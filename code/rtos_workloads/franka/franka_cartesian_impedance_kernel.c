/*
 * Per-update control computation adapted from Franka Robotics GmbH's ROS 2
 * CartesianImpedanceExampleController, jazzy commit
 * 73a1501d76efa2bc4bf09cb2af9c2b72c2c642da.
 * https://github.com/frankarobotics/franka_ros2
 *
 * Copyright (c) 2023 Franka Robotics GmbH
 * Licensed under the Apache License, Version 2.0.
 *
 * RTOS C port: ROS 2, Eigen containers, hardware access and plant dynamics are
 * intentionally absent. The damped pseudoinverse retains the upstream Jacobi
 * SVD method and damping factor.
 */

#include "franka_cartesian_impedance_kernel.h"
#include <prt_buildef.h>

#ifdef OS_OPTION_CACHECOLORING
#define FRANKA_KERNEL_TEXT __attribute__((section(".user.crit.text")))
#define FRANKA_KERNEL_DATA __attribute__((section(".user.crit.data")))
#else
#define FRANKA_KERNEL_TEXT
#define FRANKA_KERNEL_DATA
#endif

#define FRANKA_SVD_MAX_SWEEPS 16U
#define FRANKA_SVD_EPSILON 1.0e-12
#define FRANKA_PINV_DAMPING 0.2

/* Defaults declared by the pinned upstream controller. */
FRANKA_KERNEL_DATA static double g_cartesian_k[FRANKA_CARTESIAN_DOF] =
    {150.0, 150.0, 150.0, 10.0, 10.0, 10.0};
FRANKA_KERNEL_DATA static double g_cartesian_d[FRANKA_CARTESIAN_DOF] =
    {24.49489742783178, 24.49489742783178, 24.49489742783178,
     6.324555320336759, 6.324555320336759, 6.324555320336759};
FRANKA_KERNEL_DATA static double g_nullspace_k = 20.0;
FRANKA_KERNEL_DATA static double g_nullspace_d = 8.944271909999159;

FRANKA_KERNEL_TEXT static double FrankaAbs(double value)
{
    return value < 0.0 ? -value : value;
}

FRANKA_KERNEL_TEXT static double FrankaSqrt(double value)
{
    double result;
    if (value <= 0.0) {
        return 0.0;
    }
    __asm__ volatile("fsqrt %d0, %d1" : "=w"(result) : "w"(value));
    return result;
}

/* One-sided Jacobi SVD of A=J^T (7x6), sufficient for the damped inverse. */
FRANKA_KERNEL_TEXT static unsigned int FrankaDampedJacobiPinv(
    const double jacobian[FRANKA_CARTESIAN_DOF][FRANKA_JOINTS],
    double pinv[FRANKA_CARTESIAN_DOF][FRANKA_JOINTS])
{
    double b[FRANKA_JOINTS][FRANKA_CARTESIAN_DOF];
    double v[FRANKA_CARTESIAN_DOF][FRANKA_CARTESIAN_DOF];
    double sigma2[FRANKA_CARTESIAN_DOF];
    unsigned int sweep;
    int row;
    int column;
    int p;
    int q;
    int singular;

    for (row = 0; row < FRANKA_JOINTS; ++row) {
        for (column = 0; column < FRANKA_CARTESIAN_DOF; ++column) {
            b[row][column] = jacobian[column][row];
        }
    }
    for (row = 0; row < FRANKA_CARTESIAN_DOF; ++row) {
        for (column = 0; column < FRANKA_CARTESIAN_DOF; ++column) {
            v[row][column] = row == column ? 1.0 : 0.0;
        }
    }

    for (sweep = 0; sweep < FRANKA_SVD_MAX_SWEEPS; ++sweep) {
        int changed = 0;
        for (p = 0; p < FRANKA_CARTESIAN_DOF - 1; ++p) {
            for (q = p + 1; q < FRANKA_CARTESIAN_DOF; ++q) {
                double alpha = 0.0;
                double beta = 0.0;
                double gamma = 0.0;
                double threshold;
                double zeta;
                double tangent;
                double cosine;
                double sine;

                for (row = 0; row < FRANKA_JOINTS; ++row) {
                    alpha += b[row][p] * b[row][p];
                    beta += b[row][q] * b[row][q];
                    gamma += b[row][p] * b[row][q];
                }
                threshold = FRANKA_SVD_EPSILON * FrankaSqrt(alpha * beta);
                if (FrankaAbs(gamma) <= threshold) {
                    continue;
                }
                changed = 1;
                zeta = (beta - alpha) / (2.0 * gamma);
                tangent = (zeta < 0.0 ? -1.0 : 1.0) /
                    (FrankaAbs(zeta) + FrankaSqrt(1.0 + zeta * zeta));
                cosine = 1.0 / FrankaSqrt(1.0 + tangent * tangent);
                sine = cosine * tangent;

                for (row = 0; row < FRANKA_JOINTS; ++row) {
                    double bp = b[row][p];
                    double bq = b[row][q];
                    b[row][p] = cosine * bp - sine * bq;
                    b[row][q] = sine * bp + cosine * bq;
                }
                for (row = 0; row < FRANKA_CARTESIAN_DOF; ++row) {
                    double vp = v[row][p];
                    double vq = v[row][q];
                    v[row][p] = cosine * vp - sine * vq;
                    v[row][q] = sine * vp + cosine * vq;
                }
            }
        }
        if (!changed) {
            ++sweep;
            break;
        }
    }

    for (singular = 0; singular < FRANKA_CARTESIAN_DOF; ++singular) {
        sigma2[singular] = 0.0;
        for (row = 0; row < FRANKA_JOINTS; ++row) {
            sigma2[singular] += b[row][singular] * b[row][singular];
        }
    }
    for (row = 0; row < FRANKA_CARTESIAN_DOF; ++row) {
        for (column = 0; column < FRANKA_JOINTS; ++column) {
            double value = 0.0;
            for (singular = 0; singular < FRANKA_CARTESIAN_DOF; ++singular) {
                value += v[row][singular] * b[column][singular] /
                    (sigma2[singular] + FRANKA_PINV_DAMPING * FRANKA_PINV_DAMPING);
            }
            pinv[row][column] = value;
        }
    }
    return sweep;
}

FRANKA_KERNEL_TEXT static void FrankaComputePoseError(
    const FrankaCartesianInput *input,
    double error[FRANKA_CARTESIAN_DOF])
{
    double current[4];
    double desired[4];
    double dot = 0.0;
    double local_error[3];
    double rotation[3][3];
    int index;

    for (index = 0; index < 3; ++index) {
        error[index] = input->position[index] - input->desired_position[index];
    }
    for (index = 0; index < 4; ++index) {
        current[index] = input->orientation[index];
        desired[index] = input->desired_orientation[index];
        dot += current[index] * desired[index];
    }
    if (dot < 0.0) {
        for (index = 0; index < 4; ++index) {
            current[index] = -current[index];
        }
    }

    /* vector part of inverse(current) * desired */
    local_error[0] = current[0] * desired[1] - current[1] * desired[0] -
        current[2] * desired[3] + current[3] * desired[2];
    local_error[1] = current[0] * desired[2] + current[1] * desired[3] -
        current[2] * desired[0] - current[3] * desired[1];
    local_error[2] = current[0] * desired[3] - current[1] * desired[2] +
        current[2] * desired[1] - current[3] * desired[0];

    rotation[0][0] = 1.0 - 2.0 * (current[2] * current[2] + current[3] * current[3]);
    rotation[0][1] = 2.0 * (current[1] * current[2] - current[0] * current[3]);
    rotation[0][2] = 2.0 * (current[1] * current[3] + current[0] * current[2]);
    rotation[1][0] = 2.0 * (current[1] * current[2] + current[0] * current[3]);
    rotation[1][1] = 1.0 - 2.0 * (current[1] * current[1] + current[3] * current[3]);
    rotation[1][2] = 2.0 * (current[2] * current[3] - current[0] * current[1]);
    rotation[2][0] = 2.0 * (current[1] * current[3] - current[0] * current[2]);
    rotation[2][1] = 2.0 * (current[2] * current[3] + current[0] * current[1]);
    rotation[2][2] = 1.0 - 2.0 * (current[1] * current[1] + current[2] * current[2]);

    for (index = 0; index < 3; ++index) {
        error[3 + index] = -(rotation[index][0] * local_error[0] +
            rotation[index][1] * local_error[1] +
            rotation[index][2] * local_error[2]);
    }
}

FRANKA_KERNEL_TEXT void FrankaCartesianImpedanceKernelInit(FrankaCartesianKernel *kernel)
{
    int index;
    if (kernel == 0) {
        return;
    }
    for (index = 0; index < FRANKA_CARTESIAN_DOF; ++index) {
        kernel->pose_error[index] = 0.0;
    }
    for (index = 0; index < FRANKA_JOINTS; ++index) {
        kernel->tau_task[index] = 0.0;
        kernel->tau_nullspace[index] = 0.0;
        kernel->tau_command[index] = 0.0;
    }
    kernel->svd_sweeps = 0U;
    kernel->update_count = 0ULL;
}

FRANKA_KERNEL_TEXT void FrankaCartesianImpedanceKernelStep(
    FrankaCartesianKernel *kernel,
    const FrankaCartesianInput *input)
{
    double pinv[FRANKA_CARTESIAN_DOF][FRANKA_JOINTS];
    double cartesian_velocity[FRANKA_CARTESIAN_DOF];
    double wrench[FRANKA_CARTESIAN_DOF];
    double nullspace_command[FRANKA_JOINTS];
    double pinv_nullspace[FRANKA_CARTESIAN_DOF];
    int row;
    int joint;

    if (kernel == 0 || input == 0) {
        return;
    }
    FrankaComputePoseError(input, kernel->pose_error);
    kernel->svd_sweeps = FrankaDampedJacobiPinv(input->jacobian, pinv);

    for (row = 0; row < FRANKA_CARTESIAN_DOF; ++row) {
        cartesian_velocity[row] = 0.0;
        for (joint = 0; joint < FRANKA_JOINTS; ++joint) {
            cartesian_velocity[row] += input->jacobian[row][joint] * input->dq[joint];
        }
        wrench[row] = -g_cartesian_k[row] * kernel->pose_error[row] -
            g_cartesian_d[row] * cartesian_velocity[row];
    }

    for (joint = 0; joint < FRANKA_JOINTS; ++joint) {
        kernel->tau_task[joint] = 0.0;
        nullspace_command[joint] = g_nullspace_k *
            (input->q_nullspace[joint] - input->q[joint]) -
            g_nullspace_d * input->dq[joint];
        for (row = 0; row < FRANKA_CARTESIAN_DOF; ++row) {
            kernel->tau_task[joint] += input->jacobian[row][joint] * wrench[row];
        }
    }
    for (row = 0; row < FRANKA_CARTESIAN_DOF; ++row) {
        pinv_nullspace[row] = 0.0;
        for (joint = 0; joint < FRANKA_JOINTS; ++joint) {
            pinv_nullspace[row] += pinv[row][joint] * nullspace_command[joint];
        }
    }
    for (joint = 0; joint < FRANKA_JOINTS; ++joint) {
        double projected = 0.0;
        for (row = 0; row < FRANKA_CARTESIAN_DOF; ++row) {
            projected += input->jacobian[row][joint] * pinv_nullspace[row];
        }
        kernel->tau_nullspace[joint] = nullspace_command[joint] - projected;
        kernel->tau_command[joint] = kernel->tau_task[joint] +
            kernel->tau_nullspace[joint] + input->coriolis[joint];
    }
    kernel->update_count++;
}

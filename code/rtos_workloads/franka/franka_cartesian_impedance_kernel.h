#ifndef FRANKA_CARTESIAN_IMPEDANCE_KERNEL_H
#define FRANKA_CARTESIAN_IMPEDANCE_KERNEL_H

#ifdef __cplusplus
extern "C" {
#endif

#define FRANKA_JOINTS 7
#define FRANKA_CARTESIAN_DOF 6

/* One deterministic snapshot of the interfaces read by the Franka controller. */
typedef struct {
    double q[FRANKA_JOINTS];
    double dq[FRANKA_JOINTS];
    double position[3];
    double orientation[4];       /* unit quaternion: w, x, y, z */
    double desired_position[3];
    double desired_orientation[4];
    double q_nullspace[FRANKA_JOINTS];
    double coriolis[FRANKA_JOINTS];
    double jacobian[FRANKA_CARTESIAN_DOF][FRANKA_JOINTS];
} FrankaCartesianInput;

typedef struct {
    double pose_error[FRANKA_CARTESIAN_DOF];
    double tau_task[FRANKA_JOINTS];
    double tau_nullspace[FRANKA_JOINTS];
    double tau_command[FRANKA_JOINTS];
    unsigned int svd_sweeps;
    unsigned long long update_count;
} FrankaCartesianKernel;

void FrankaCartesianImpedanceKernelInit(FrankaCartesianKernel *kernel);

/* Execute exactly one controller update for one timer-triggered activation. */
void FrankaCartesianImpedanceKernelStep(
    FrankaCartesianKernel *kernel,
    const FrankaCartesianInput *input);

#ifdef __cplusplus
}
#endif

#endif

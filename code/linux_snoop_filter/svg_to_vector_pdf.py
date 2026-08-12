#!/usr/bin/env python3
"""Convert the project's simple publication SVG figures to vector PDF.

The dependency-free plotter emits only rect, line, circle, polyline and text
elements, so a small explicit converter avoids rasterizing the paper figure.
"""

from __future__ import annotations

import argparse
import re
import xml.etree.ElementTree as ET
from pathlib import Path

from reportlab.pdfbase.pdfmetrics import stringWidth
from reportlab.pdfgen import canvas
from reportlab.lib.colors import HexColor


def number(value: str | None, default: float = 0.0) -> float:
    if value is None:
        return default
    match = re.match(r"[-+0-9.eE]+", value)
    return float(match.group(0)) if match else default


def color(value: str | None, default: str = "#000000"):
    if not value or value == "none":
        return None
    if value == "white":
        value = "#FFFFFF"
    return HexColor(value if value.startswith("#") else default)


def apply_stroke(pdf, element):
    stroke = color(element.get("stroke"))
    if stroke is None:
        return False
    pdf.setStrokeColor(stroke)
    pdf.setLineWidth(number(element.get("stroke-width"), 1.0))
    dash = element.get("stroke-dasharray")
    pdf.setDash([number(part) for part in dash.split(",")]) if dash else pdf.setDash()
    return True


def convert(svg_path: Path, pdf_path: Path, width_inches: float):
    root = ET.parse(svg_path).getroot()
    width = number(root.get("width"))
    height = number(root.get("height"))
    page_width = width_inches * 72.0
    scale = page_width / width
    page_height = height * scale
    pdf = canvas.Canvas(str(pdf_path), pagesize=(page_width, page_height), pageCompression=1)
    pdf.scale(scale, scale)

    for element in root:
        tag = element.tag.rsplit("}", 1)[-1]
        if tag == "rect":
            fill = color(element.get("fill"))
            stroke_ok = apply_stroke(pdf, element)
            x = number(element.get("x"))
            y = number(element.get("y"))
            w = number(element.get("width"), width if element.get("width") == "100%" else 0)
            h = number(element.get("height"), height if element.get("height") == "100%" else 0)
            if element.get("width") == "100%":
                w = width
            if element.get("height") == "100%":
                h = height
            if fill:
                pdf.setFillColor(fill)
            pdf.rect(x, height - y - h, w, h, fill=int(fill is not None), stroke=int(stroke_ok))
        elif tag == "line":
            if apply_stroke(pdf, element):
                pdf.line(number(element.get("x1")), height - number(element.get("y1")),
                         number(element.get("x2")), height - number(element.get("y2")))
        elif tag == "circle":
            fill = color(element.get("fill"))
            stroke_ok = apply_stroke(pdf, element)
            if fill:
                pdf.setFillColor(fill)
            pdf.circle(number(element.get("cx")), height - number(element.get("cy")),
                       number(element.get("r")), fill=int(fill is not None), stroke=int(stroke_ok))
        elif tag == "polyline":
            if not apply_stroke(pdf, element):
                continue
            points = [tuple(map(float, pair.split(","))) for pair in element.get("points", "").split()]
            if not points:
                continue
            path = pdf.beginPath()
            path.moveTo(points[0][0], height - points[0][1])
            for x, y in points[1:]:
                path.lineTo(x, height - y)
            pdf.drawPath(path, stroke=1, fill=0)
        elif tag == "text":
            value = "".join(element.itertext())
            size = number(element.get("font-size"), 13)
            font = "Helvetica-Bold" if element.get("font-weight") == "bold" else "Helvetica"
            fill = color(element.get("fill"), "#222222") or HexColor("#222222")
            anchor = element.get("text-anchor", "start")
            offset = 0.0
            text_width = stringWidth(value, font, size)
            if anchor == "middle":
                offset = -text_width / 2
            elif anchor == "end":
                offset = -text_width
            x = number(element.get("x"))
            y = number(element.get("y"))
            pdf.saveState()
            pdf.setFillColor(fill)
            pdf.setFont(font, size)
            transform = element.get("transform", "")
            rotate = re.search(r"rotate\(([-+0-9.]+)", transform)
            if rotate:
                pdf.translate(x, height - y)
                pdf.rotate(-float(rotate.group(1)))
                pdf.drawString(offset, 0, value)
            else:
                pdf.drawString(x + offset, height - y, value)
            pdf.restoreState()

    pdf.showPage()
    pdf.save()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("svg", type=Path)
    parser.add_argument("pdf", type=Path)
    parser.add_argument("--width-inches", type=float, default=7.16,
                        help="Final PDF width; 7.16 in matches a typical IEEE two-column figure")
    args = parser.parse_args()
    convert(args.svg, args.pdf, args.width_inches)


if __name__ == "__main__":
    main()

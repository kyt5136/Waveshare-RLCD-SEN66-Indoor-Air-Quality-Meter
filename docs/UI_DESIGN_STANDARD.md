# LCD User Interface Design Standard

## 1. Purpose

This document defines the LCD interface for the 400 by 300 pixel reflective display.

Use this standard for the Arduino firmware and the future ESP-IDF port.
Do not create a separate visual system for the ESP-IDF port.

## 2. Display properties

The ST7305 display uses one-bit pixels.
The interface uses black and white only.
The display has no backlight.

Use solid fills, strong outlines, and large text.
Do not use gray patterns to communicate essential information.

## 3. Page frame

Each normal page uses the same frame.

| Item | Position and size |
|---|---|
| Canvas | 400 by 300 pixels |
| Outer border | `x=0`, `y=0`, `w=400`, `h=300` |
| Inner border | `x=1`, `y=1`, `w=398`, `h=298` |
| Header bar | `x=8`, `y=8`, `w=384`, `h=26` |
| Content start | `y=42` |
| Normal side margin | 8 pixels |

The header bar uses a solid black fill.
The header uses white text.
The page title starts seven pixels from the left edge.
The local time ends seven pixels from the right edge.

Do not put coordinates, location strings, status details, or units in the header.
Show the active location on the web interface.

## 4. Typography

Use `FreeSansBold9pt7b` for labels, status text, and header text.
Use `FreeSansBold12pt7b` for normal values.
Use `FreeSans18pt7b` or `FreeSans24pt7b` for primary values.

Use DSEG7 only for the timer and stopwatch digits.
Do not use DSEG7 for measurements or labels.

Do not wrap text on the LCD.
Shorten a label before it reaches a box boundary.
Use the same abbreviation on every page.

## 5. Measurement boxes

Use equal widths for boxes in one row.
Use equal gaps between those boxes.
Use one label baseline for the complete row.
Use one value baseline for the complete row.

Center short labels and values.
Use left alignment only for tables or descriptive text.

Put the unit in the label when this makes the value larger.
Examples include `CO2 (ppm)`, `TEMP (F)`, and `RH (%)`.

Do not repeat the unit beside a large value.
Do not reduce the value font to fit a repeated unit.

## 6. Page titles

Use these titles:

| Page | Header title |
|---:|---|
| 0 | `INDOOR AIR` |
| 1 | `ANALOG CLOCK` |
| 2 | `NORTH AMERICAN TIME` |
| 3 | `CURRENT CONDITIONS` |
| 4 | `NEXT 6 HOURS` |
| 5 | `NEXT 60 MINUTES` |
| 6 | `3-DAY FORECAST` |
| 7 | `SEASONS` |
| 8 | `SEASON ORBIT` |
| 9 | `INDOOR TEMPERATURE` |
| 10 | `INDOOR HUMIDITY` |
| 11 | `SYSTEM INFORMATION` |
| 12 | `SEN66 SENSOR OUTPUT` |
| 13 | `TIMERS & ALARMS` |

## 7. Data presentation

Show all user-facing temperatures in degrees Fahrenheit.
Show all user-facing wind speeds in miles per hour.
Show carbon dioxide in parts per million.
Show particulate mass in micrograms per cubic meter.

Use `--` when a value is unavailable.
Do not show a zero when the sensor reports an invalid value.

Show the latest valid local time in every header.
Keep the time position fixed on every page.

## 8. Status text

Use short status phrases.
Use `SEN66: WAITING`, `SEN66: PREHEATING`, and `SEN66: READY`.

Do not use a font smaller than the label font.
Do not place status text closer than six pixels to a border.

## 9. Graphs

Use the complete graph width for the time series.
Keep axis labels outside the plotting rectangle.
Use the same current-value position on both history pages.

Do not connect missing samples.
Keep minimum and maximum markers inside the plot area.

## 10. ESP-IDF handoff

Create one reusable page-frame function.
Create one reusable metric-box function.
Create one reusable text-alignment function.

Keep page geometry in named constants.
Do not copy header geometry into each page.

Use the same font files or verified metric-compatible replacements.
Compare text bounds before you approve a replacement font.

Render each page to a 400 by 300 test image.
Check every page with maximum-length values.
Check every page with unavailable values.

## 11. Acceptance criteria

Each page must have the standard black header.
Each header must show a title and local time.
No header text can wrap or overlap.

All repeated boxes must use equal spacing.
All values in one box row must use the same baseline.
All small text must remain legible on the physical RLCD.

The firmware must compile with the production partition settings.
The physical display test must include all 14 pages.

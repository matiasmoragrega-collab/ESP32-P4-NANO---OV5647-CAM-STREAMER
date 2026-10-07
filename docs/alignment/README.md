# Camera alignment overlays

Both images are 1280x960 RGBA, i.e. the same size as the camera frame. They are meant to be laid over the live preview.

| File | What it is |
|---|---|
| `target-overlay.png` | The target view (the content of the Holoviz window, without the title bar) resampled into camera coordinates. It is fully opaque wherever the target has content. A 27 px strip on the left and a 24 px strip on the right are transparent. The dashed guide lines are drawn on top. The diagram's own dotted lines were inpainted out of the target image. |
| `target-guides.png` | Transparent image with only the guide lines. It also has a dashed white/grey 2 px outline showing where the edges of the target content land; only the left and right sides fall inside the frame. |

## How to use
1. In the web UI, open the settings panel, then **Overlay image → Choose image**.
2. Set **Fit = "Stretch to fill"** and keep the preview rotation at **0°**.
3. Set the opacity:
   - `target-overlay.png`: about **40–50 %**. Adjust the camera until the live grippers and scene match the ghosted target.
   - `target-guides.png`: **100 %**. Adjust the camera until the grippers sit on the lines.

## Mapping (measured from the reference diagram, 952x857 px)
- Target window content: x 33..454, y 75..494. The title bar spans y 51..73. Column 32 and rows 74 and 494 are anti-aliased edge pixels.
- Current camera frame, top-right copy: x 480..918, y 121..450. That is 438x329, which is 4:3.
- Current camera frame, bottom-left copy: x 24..462, y 500..829. Same size, 438x329.
- Camera frame in diagram coordinates: left = 24, top = 121, width 438, height 329.
- `cx = (sx - 24) / 438 * 1280` and `cy = (sy - 121) / 329 * 960`. The scale is about 2.92 camera px per diagram px on both axes, so the image is not distorted.
- The target content maps to camera x 26.3..1256.6 and y −134.2..1088.4. About 134 px of the target above the frame and 128 px below it are cut off.

## Guide lines (camera pixels, each drawn 3 px wide)
| Line | Diagram | Camera |
|---|---|---|
| Red row 1 | y 382.5 | **y = 763** |
| Red row 2 | y 430.1 | **y = 902** |
| Red row 3 (target bottom edge) | y 494.3 | y = 1089, outside the frame, not drawn |
| Green column | x 55.8 | **x = 93** |
| Red column | x 435.0 | **x = 1201** |

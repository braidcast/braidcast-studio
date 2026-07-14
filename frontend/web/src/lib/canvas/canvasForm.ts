import type { CanvasInfo, CanvasUpdateParams } from "$lib/api/bridge";

/** Integer frame-rate presets; fractional rates (59.94 = 60000/1001) are Custom. */
export const FPS_PRESETS = [24, 30, 48, 60, 120];

/** The full editable state of one canvas's detail pane. `fpsCustom` tracks whether
 * the frame rate is a non-preset value (drives the conditional Custom input). */
export interface CanvasForm {
  name: string;
  width: number;
  height: number;
  fpsNum: number;
  fpsDen: number;
  fpsCustom: boolean;
  scaleType: string;
  useDefaultRes: boolean;
  videoEnc: string;
  audioEnc: string;
  videoUseDefault: boolean;
  audioUseDefault: boolean;
  colorFormat: string;
  colorSpace: string;
  colorRange: string;
  sdrWhiteLevel: number;
  hdrNominalPeakLevel: number;
  colorUseDefault: boolean;
}

export function seedForm(c: CanvasInfo): CanvasForm {
  return {
    name: c.name,
    width: c.baseWidth,
    height: c.baseHeight,
    fpsNum: c.fpsNum,
    fpsDen: c.fpsDen,
    fpsCustom: !(c.fpsDen === 1 && FPS_PRESETS.includes(c.fpsNum)),
    scaleType: c.scaleType || "bicubic",
    useDefaultRes: c.useDefaultResolution,
    videoEnc: c.videoEncoder,
    audioEnc: c.audioEncoder,
    videoUseDefault: c.videoUseDefault,
    audioUseDefault: c.audioUseDefault,
    colorFormat: c.color.format,
    colorSpace: c.color.space,
    colorRange: c.color.range,
    sdrWhiteLevel: c.color.sdrWhiteLevel,
    hdrNominalPeakLevel: c.color.hdrNominalPeakLevel,
    colorUseDefault: c.color.useDefault,
  };
}

/** Build the canvas.update payload from the form. Output resolution is always
 * mirror-of-base (0/0) — the redesign dropped the separate scaled-output field. */
export function toUpdateParams(uuid: string, f: CanvasForm): CanvasUpdateParams {
  return {
    uuid,
    name: f.name.trim(),
    baseWidth: f.width,
    baseHeight: f.height,
    outputWidth: 0,
    outputHeight: 0,
    fpsNum: f.fpsNum,
    fpsDen: f.fpsDen,
    scaleType: f.scaleType,
    useDefaultResolution: f.useDefaultRes,
    videoEncoder: f.videoEnc || undefined,
    audioEncoder: f.audioEnc || undefined,
    videoUseDefault: f.videoUseDefault,
    audioUseDefault: f.audioUseDefault,
    color: {
      format: f.colorFormat,
      space: f.colorSpace,
      range: f.colorRange,
      sdrWhiteLevel: f.sdrWhiteLevel,
      hdrNominalPeakLevel: f.hdrNominalPeakLevel,
      useDefault: f.colorUseDefault,
    },
  };
}

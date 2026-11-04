import { useEffect, useRef } from "react";
export function PopulationThumbnail({ pixels }: { pixels: number[] }) {
  const canvas = useRef<HTMLCanvasElement>(null);
  useEffect(() => {
    if (pixels.length !== 4096) return;
    const context = canvas.current?.getContext("2d");
    if (!context) return;
    const image = context.createImageData(64, 64);
    pixels.forEach((v, i) => { image.data[4*i] = v; image.data[4*i+1] = v; image.data[4*i+2] = v; image.data[4*i+3] = 255; });
    context.putImageData(image, 0, 0);
  }, [pixels]);
  return <canvas ref={canvas} width={64} height={64} aria-label="Selected observation thumbnail" />;
}

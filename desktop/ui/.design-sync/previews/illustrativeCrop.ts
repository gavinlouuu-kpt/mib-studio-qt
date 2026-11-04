// Illustrative crops for the kit previews only: a grey field with one cell,
// plus the matching outline in image pixels. Real crops come from the backend.
export function illustrativeCrop(seed: number, size = 80) {
  const r = (n: number) => {
    const x = Math.sin(seed * 97 + n * 13.7) * 10000;
    return x - Math.floor(x);
  };
  const cx = size / 2 + (r(1) - 0.5) * size * 0.1;
  const cy = size / 2 + (r(2) - 0.5) * size * 0.1;
  const rx = size * (0.26 + r(3) * 0.08);
  const ry = rx * (0.86 + r(4) * 0.12);
  const svg =
    `<svg xmlns="http://www.w3.org/2000/svg" width="${size}" height="${size}" viewBox="0 0 ${size} ${size}">` +
    `<rect width="${size}" height="${size}" fill="#dce3e3"/>` +
    `<ellipse cx="${cx}" cy="${cy}" rx="${rx}" ry="${ry}" fill="#b7c2c2"/>` +
    `<circle cx="${cx + rx * 0.2}" cy="${cy - ry * 0.25}" r="${rx * 0.28}" fill="#8e9b9b"/></svg>`;
  const mask =
    `<svg xmlns="http://www.w3.org/2000/svg" width="${size}" height="${size}" viewBox="0 0 ${size} ${size}">` +
    `<ellipse cx="${cx}" cy="${cy}" rx="${rx}" ry="${ry}" fill="#2f6e6a"/></svg>`;
  const contour: Array<[number, number]> = Array.from({ length: 32 }, (_, i) => {
    const a = (i / 32) * Math.PI * 2;
    return [Math.round((cx + rx * Math.cos(a)) * 10) / 10, Math.round((cy + ry * Math.sin(a)) * 10) / 10];
  });
  return {
    src: `data:image/svg+xml;utf8,${encodeURIComponent(svg)}`,
    maskSrc: `data:image/svg+xml;utf8,${encodeURIComponent(mask)}`,
    contour,
    imageWidth: size,
    imageHeight: size,
  };
}

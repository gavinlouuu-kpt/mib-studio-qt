---
category: Data
---

# CellImage

One recorded cell crop with its recorded contour or mask, identified by its record (`recordId`, a string). Use it for the inspected record in Review (R3) and, through `CellStrip`, for the live detected-cell strip (O4). Pixels are shown as recorded, without smoothing.

The component draws only what was recorded. Without `src` it says "Image not recorded". When the requested `overlay` was not recorded (no `contour`, or no `maskSrc`), a grey chip says "Contour not recorded" or "Mask not recorded". It never draws an estimated outline.

`contour` is in image pixels (`imageWidth` × `imageHeight`), so it lines up at any display `size`. Give `caption` to show the record id and a `detail` line under the crop. `selected` draws the crop in the inspect colour; `onSelect` makes it a button that reports the record id.

## Example

```tsx
<CellImage
  recordId="R-024210"
  src={cropUrl}
  imageWidth={80}
  imageHeight={80}
  contour={recordedContour}
  overlay="contour"
  label="CONTOUR"
  size={150}
  caption
  detail="frame 82 140 · object 02"
/>
```

## Penpot reference

Drawn in the Penpot sandbox file `2c3c14b3-8959-40f7-a29c-2e6202e341b6` (record images and the detected-cell strip):

- C · CellImage: page `b64e9247-17fe-514c-be52-6d82bead2b2c`, board `3dff16a8-610c-45d8-b069-fb300f3c056b`
- R3 · Inspect: page `6dcbad3e-bdc3-5c94-9a59-1307866b24c8`, board `dcdbaa6b-9790-4e69-bea4-936747f588b1`
- O4 · Experiment · Live: page `abaacf41-dfaf-500a-bc29-e6aa946d3273`, board `22f62c97-3b47-43f4-bb9c-96eb2be8c257`

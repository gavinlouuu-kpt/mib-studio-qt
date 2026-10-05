import {useEffect, useRef, useState} from 'react';
import {bridge} from '../bridge';
import {decodeProcessedPreview, type ProcessedPreview as Snapshot} from '../processedPreview';
export function ProcessedPreview({ready, active = true}: {ready: boolean; active?: boolean}) {
  const canvas = useRef<HTMLCanvasElement>(null), snapshot = useRef<Snapshot | null>(null);
  const [meta, setMeta] = useState<Snapshot | null>(null), [error, setError] = useState('');
  const [target, setTarget] = useState(true);
  const [roi, setRoi] = useState(true), [mask, setMask] = useState(false), [contours, setContours] = useState(true);
  const options = useRef({roi, mask, contours, target}); options.current = {roi, mask, contours, target};
  function draw(frame: Snapshot) {
    const element = canvas.current; if (!element) return;
    element.width = frame.width; element.height = frame.height;
    const ctx = element.getContext('2d'); if (!ctx) return;
    const image = ctx.createImageData(frame.width, frame.height);
    for (let i = 0; i < frame.pixels.length; i++) {
      const overlay = options.current.mask && frame.mask[i] > 0;
      image.data[4*i] = overlay ? Math.round(frame.pixels[i] * .55) : frame.pixels[i];
      image.data[4*i+1] = overlay ? Math.round(frame.pixels[i] * .55 + 110) : frame.pixels[i];
      image.data[4*i+2] = frame.pixels[i]; image.data[4*i+3] = 255;
    }
    ctx.putImageData(image, 0, 0); ctx.lineWidth = 1;
    if (options.current.target && frame.primary_object_target) {ctx.strokeStyle = '#ff3070'; ctx.lineWidth = 2; ctx.strokeRect(frame.primary_bounds[0],frame.primary_bounds[1],frame.primary_bounds[2],frame.primary_bounds[3]); ctx.lineWidth = 1;}
    if (options.current.roi) {ctx.strokeStyle = '#ffcc00'; ctx.strokeRect(frame.roi[0], frame.roi[1], frame.roi[2], frame.roi[3]);}
    if (options.current.contours) {ctx.strokeStyle = '#00e5ff'; for (const line of frame.contours) {if (!line.length) continue; ctx.beginPath(); ctx.moveTo(line[0][0], line[0][1]); for (const point of line.slice(1)) ctx.lineTo(point[0], point[1]); ctx.closePath(); ctx.stroke();}}
  }
  useEffect(() => {if (snapshot.current) draw(snapshot.current);}, [roi, mask, contours, target]);
  useEffect(() => {
    if (!ready || !active) return;
    let alive = true, pending = false;
    const poll = async () => {if (!alive || pending) return; pending = true; try {
      const frame = decodeProcessedPreview(await bridge.fetchProcessedPreview());
      if (!alive) return; snapshot.current = frame; setMeta(frame); setError(''); if (frame) draw(frame); else if (canvas.current) canvas.current.width = 0;
    } catch (failure) {if (alive) setError(String(failure));} finally {pending = false;}};
    void bridge.setProcessedPreviewEnabled(true).then(poll).catch(failure => {if (alive) setError(String(failure));});
    const timer = window.setInterval(() => void poll(), 200);
    return () => {alive = false; window.clearInterval(timer); void bridge.setProcessedPreviewEnabled(false).catch(() => {});};
  }, [ready, active]);
  return <fieldset><legend>Processed preview · frame-coherent overlays</legend>
    <label><input type="checkbox" checked={target} onChange={e => setTarget(e.target.checked)} />Primary target</label>
    <label><input type="checkbox" checked={roi} onChange={e => setRoi(e.target.checked)} />ROI</label>
    <label><input type="checkbox" checked={mask} onChange={e => setMask(e.target.checked)} />Mask</label>
    <label><input type="checkbox" checked={contours} onChange={e => setContours(e.target.checked)} />Contours</label>
    {error && <p role="alert">{error}</p>}
    {!meta && <p>Waiting for a processed frame with retained source pixels.</p>}
    {meta?.capture_stale && <p role="status">Retained frame from a stopped or previous capture session.</p>}
    {meta && <p>Capture {meta.capture_session} / processing {meta.processing_session} / store {meta.store_generation} / frame {meta.frame_index} · primary object {meta.primary_object_target ? 'TARGET' : meta.primary_object_valid ? 'valid' : 'invalid'}{meta.contours_truncated ? ' · contour display truncated' : ''}<br />Recipe {meta.recipe_sha256}</p>}
    <canvas ref={canvas} style={{maxWidth: '100%', imageRendering: 'pixelated'}} />
  </fieldset>;
}

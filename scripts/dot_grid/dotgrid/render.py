"""Synthetic camera-view renderer for testing the decoder without hardware."""
from __future__ import annotations

from dataclasses import dataclass
from typing import Optional, Sequence, Tuple

import cv2
import numpy as np

from .codebook import Codebook


@dataclass
class ViewPose:
    """Camera pose over the wafer. Image pixel (u, v) maps to wafer um via:
    wafer = centre_um + R(theta) * S * (u - w/2, v - h/2) * um_per_px,
    where S mirrors x when mirrored=True (viewing through the glass side)."""
    centre_um: Tuple[float, float]
    theta_deg: float = 0.0
    um_per_px: float = 0.293
    mirrored: bool = False
    width: int = 1920
    height: int = 1200

    def matrix(self) -> np.ndarray:
        """2x3 affine mapping pixel -> wafer um."""
        t = np.radians(self.theta_deg)
        c, s = np.cos(t), np.sin(t)
        R = np.array([[c, -s], [s, c]])
        S = np.diag([-1.0 if self.mirrored else 1.0, 1.0]) * self.um_per_px
        A = R @ S
        offset = np.array(self.centre_um) - A @ np.array([self.width / 2, self.height / 2])
        return np.hstack([A, offset[:, None]])

    def inverse(self) -> np.ndarray:
        M = self.matrix()
        A = M[:, :2]
        Ai = np.linalg.inv(A)
        return np.hstack([Ai, (-Ai @ M[:, 2])[:, None]])


def render_view(cb: Codebook, pose: ViewPose, *, missing: Optional[Sequence[Tuple[int, int]]] = None,
                keepout_mask=None, background: int = 180, dot_level: int = 40,
                blur_sigma_px: float = 1.5, noise_sigma: float = 6.0,
                channel_lines_um: Sequence[Tuple[Tuple[float, float], Tuple[float, float], float]] = (),
                seed: int = 0) -> np.ndarray:
    """Render the dots visible in the view as dark discs on a grey background (8-bit)."""
    img = np.full((pose.height, pose.width), background, np.uint8)
    inv = pose.inverse()
    # wafer bbox of the view
    corners = np.array([[0, 0], [pose.width, 0], [0, pose.height], [pose.width, pose.height]], float)
    M = pose.matrix()
    wc = corners @ M[:, :2].T + M[:, 2]
    pitch = cb.pitch_um
    i0 = int(np.floor((wc[:, 0].min() - cb.origin_um[0]) / pitch)) - 1
    i1 = int(np.ceil((wc[:, 0].max() - cb.origin_um[0]) / pitch)) + 1
    j0 = int(np.floor((wc[:, 1].min() - cb.origin_um[1]) / pitch)) - 1
    j1 = int(np.ceil((wc[:, 1].max() - cb.origin_um[1]) / pitch)) + 1
    missing_set = set(missing or ())
    r_px = cb.dot_diameter_um / 2 / pose.um_per_px
    for i in range(max(i0, 0), min(i1, cb.columns - 1) + 1):
        for j in range(max(j0, 0), min(j1, cb.rows - 1) + 1):
            if (i, j) in missing_set:
                continue
            x, y = cb.dot_um(i, j)
            if keepout_mask is not None and not keepout_mask(x, y):
                continue
            p = inv[:, :2] @ np.array([x, y]) + inv[:, 2]
            if -r_px <= p[0] < pose.width + r_px and -r_px <= p[1] < pose.height + r_px:
                cv2.circle(img, (int(round(p[0] * 16)), int(round(p[1] * 16))), int(round(r_px * 16)),
                           dot_level, -1, lineType=cv2.LINE_AA, shift=4)
    for (a, b, width_um) in channel_lines_um:
        pa = inv[:, :2] @ np.array(a) + inv[:, 2]
        pb = inv[:, :2] @ np.array(b) + inv[:, 2]
        w = max(1, int(round(width_um / pose.um_per_px)))
        cv2.line(img, (int(pa[0]), int(pa[1])), (int(pb[0]), int(pb[1])), dot_level + 40, w, cv2.LINE_AA)
    if blur_sigma_px > 0:
        img = cv2.GaussianBlur(img, (0, 0), blur_sigma_px)
    if noise_sigma > 0:
        rng = np.random.default_rng(seed)
        img = np.clip(img.astype(np.float32) + rng.normal(0, noise_sigma, img.shape), 0, 255).astype(np.uint8)
    return img

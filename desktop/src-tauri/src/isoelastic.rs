//! Isoelastic reference curves (deformability vs area at constant Young's
//! modulus) for the review scatter (plan 2026-10-01-standalone-review-app,
//! PR 3). The Qt tab reads `resources/isoelastic_curve/…` next to its
//! executable; the Tauri products embed the same file at compile time, so
//! no bundle path can be missing on any platform.
//!
//! Format: `#` comment / metadata lines, then tab-separated
//! `area_um2  deformability  emodulus_kPa` rows; one curve per modulus.

use serde::Serialize;

const DATA: &str = include_str!("../../../resources/isoelastic_curve/scaled_isoelastic_data_6.16-4.24.txt");

#[derive(Serialize, Clone, Debug, PartialEq)]
pub struct Curve {
    pub emodulus_kpa: f64,
    /// (area µm², deformability), file order.
    pub points: Vec<(f64, f64)>,
}

#[derive(Serialize, Clone, Debug)]
pub struct Curves {
    pub source: &'static str,
    pub curves: Vec<Curve>,
}

/// Parse the curve file; curves sorted by descending modulus (the Qt legend
/// order).
pub fn parse(text: &str) -> Vec<Curve> {
    let mut curves: Vec<Curve> = Vec::new();
    for line in text.lines() {
        let line = line.trim();
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        let mut parts = line.split('\t').filter(|p| !p.is_empty());
        let (Some(a), Some(d), Some(e)) = (parts.next(), parts.next(), parts.next()) else { continue };
        let (Ok(a), Ok(d), Ok(e)) = (a.parse::<f64>(), d.parse::<f64>(), e.parse::<f64>()) else { continue };
        if !(a.is_finite() && d.is_finite() && e.is_finite()) {
            continue;
        }
        match curves.iter_mut().find(|c| c.emodulus_kpa == e) {
            Some(c) => c.points.push((a, d)),
            None => curves.push(Curve { emodulus_kpa: e, points: vec![(a, d)] }),
        }
    }
    curves.sort_by(|x, y| y.emodulus_kpa.partial_cmp(&x.emodulus_kpa).unwrap_or(std::cmp::Ordering::Equal));
    curves
}

#[tauri::command]
pub fn fetch_isoelastic_curves() -> Curves {
    Curves { source: "scaled_isoelastic_data_6.16-4.24.txt", curves: parse(DATA) }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn embedded_file_has_eleven_finite_curves() {
        let curves = parse(DATA);
        assert_eq!(curves.len(), 11);
        assert!(curves.windows(2).all(|w| w[0].emodulus_kpa > w[1].emodulus_kpa), "descending modulus");
        for c in &curves {
            assert!(c.points.len() > 10, "{} kPa has {} points", c.emodulus_kpa, c.points.len());
            assert!(c.points.iter().all(|(a, d)| a.is_finite() && d.is_finite() && *a > 0.0));
        }
    }

    #[test]
    fn parser_skips_comments_and_malformed_rows() {
        let text = "# meta\n\n1\t0.1\t2.0\nbad row\n2\t0.2\t2.0\n3\t0.3\t1.0\n4\tx\t1.0\n";
        let curves = parse(text);
        assert_eq!(curves.len(), 2);
        assert_eq!(curves[0], Curve { emodulus_kpa: 2.0, points: vec![(1.0, 0.1), (2.0, 0.2)] });
        assert_eq!(curves[1].points, vec![(3.0, 0.3)]);
    }
}

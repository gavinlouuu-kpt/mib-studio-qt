"""Python bindings for mib-studio-qt's Qt-free processing core.

Wraps the deformability-cytometry pipeline (``ProcessingService``,
``EModulusLut``, ``BatchMaskSources``) so a non-Qt consumer -- e.g. Biowork's
``services/mib-processing`` runtime -- can run the exact same algorithm the
desktop app runs, without a Qt toolchain.

See ``docs/gold_standard_metrics.md`` in the mib-studio-qt repository ("
Portable Processing Contract") for the JSON field-name contract every dict
returned here follows, and the ``ProcessingConfig`` field table for the
flat config dict this module accepts (note: it is *flat* -- it does not
replicate the nested ``image_processing``/``filters``/``target_group``/
``multi_image`` grouping used by the desktop app's
``resources/defaults/config.json``; flatten that structure before calling
into this module).
"""

from __future__ import annotations

from ._mib_processing import (
    CONTRACT_VERSION,
    EModulusLut,
    compute_processed_frame,
    config_from_dict,
    load_from_avi,
    load_from_folder,
    load_images_from_hdf5,
    process_batch,
    save_masks_to_hdf5,
)

__version__ = "0.2.1"
#: Default ProcessingConfig, taken from the C++ struct's own field defaults
#: (include/backend/processing/ProcessingService.h), not from the desktop
#: app's resources/defaults/config.json. Callers should generally pull the
#: real config from the synced profile catalog (see the Biowork portability
#: epic's A4/B2 issues) rather than relying on these defaults for anything
#: beyond local testing.
DEFAULT_PROCESSING_CONFIG = config_from_dict({})

__all__ = [
    "CONTRACT_VERSION",
    "DEFAULT_PROCESSING_CONFIG",
    "EModulusLut",
    "compute_processed_frame",
    "config_from_dict",
    "load_from_avi",
    "load_from_folder",
    "load_images_from_hdf5",
    "process_batch",
    "save_masks_to_hdf5",
    "__version__",
]

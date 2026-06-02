"""3D orientation cube display using PyQtGraph OpenGL.

Lazy-initializes on first use — importing this module does not
start a Qt application or create windows.
"""

import math

import numpy as np
import pyqtgraph.opengl as gl
from pyqtgraph.Qt import QtWidgets

# ── Scene data (always available) ──────────────────────────────────

_verts = np.array(
    [
        [1, 1, 1],
        [-1, 1, 1],
        [-1, -1, 1],
        [1, -1, 1],
        [1, 1, -1],
        [-1, 1, -1],
        [-1, -1, -1],
        [1, -1, -1],
    ]
)

_faces = np.array(
    [
        [0, 1, 2],
        [0, 2, 3],
        [4, 5, 6],
        [4, 6, 7],
        [0, 1, 5],
        [0, 5, 4],
        [2, 3, 7],
        [2, 7, 6],
        [1, 2, 6],
        [1, 6, 5],
        [0, 3, 7],
        [0, 7, 4],
    ]
)

_colors = np.ones((_faces.shape[0], 4))
_colors[:, 0] = np.linspace(0, 1, _faces.shape[0])

# ── Lazy-initialized globals ──────────────────────────────────────

_app = None
_view = None
_mesh = None


def _init_gl():
    """Create Qt application and GL view (called once by start())."""
    global _app, _view
    if _app is not None:
        return  # already initialized

    _app = QtWidgets.QApplication.instance()
    if _app is None:
        _app = QtWidgets.QApplication([])

    _view = gl.GLViewWidget()
    _view.show()
    _view.setCameraPosition(distance=5)


# ── Orientation math ──────────────────────────────────────────────


def _quat_to_axis_angle(quat):
    """Convert quaternion [w, x, y, z] to (angle_degrees, axis)."""
    w, x, y, z = quat
    if w > 1.0:
        quat = quat / np.linalg.norm(quat)
        w, x, y, z = quat

    angle = 2.0 * math.acos(w)
    s = math.sqrt(1.0 - w * w)
    if s < 1e-8:
        axis = np.array([1.0, 0.0, 0.0])
    else:
        axis = np.array([x, y, z]) / s
    return np.degrees(angle), axis


# ── Public API ────────────────────────────────────────────────────


def update(quat):
    """Rotate the cube to match orientation quaternion [w, x, y, z].

    Call this from the IMU reader thread.  Safe to call before start() —
    the mesh is created on first call.
    """
    global _mesh

    if _view is None:
        return  # not started yet

    if _mesh is None:
        _mesh = gl.GLMeshItem(
            vertexes=_verts,
            faces=_faces,
            faceColors=_colors,
            smooth=False,
            drawEdges=False,
        )
        _view.addItem(_mesh)

    angle, axis = _quat_to_axis_angle(quat)
    _mesh.resetTransform()
    _mesh.rotate(angle, axis[0], axis[1], axis[2])


def start():
    """Start the Qt event loop (blocking)."""
    _init_gl()
    if _app is not None:
        _app.exec()

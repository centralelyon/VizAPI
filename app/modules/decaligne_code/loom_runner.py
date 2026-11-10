"""Compatibility entry point for the embedded C++ OCTI + LOOM pipeline."""
def run_octi(graph):
    from .edit_sessions import core_call
    session = core_call({"op": "session", "map": graph, "deferRender": True})
    result = core_call({"op": "loom", "state": session["state"]})
    return core_call({"op": "export", "state": result["state"]})["map"]

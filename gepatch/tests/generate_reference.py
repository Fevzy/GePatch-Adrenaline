"""Extract immutable upstream GE parser for the rollback regression check."""
from pathlib import Path
import re
root = Path(__file__).resolve().parents[2]
source = (root / "gepatch/upstream/main.c").read_text()
part = source[source.index("static const u8 tcsize"):source.index("\nvoid *(* _sceGeEdramGetAddr)")]
for name in ["tcsize", "tcalign", "colsize", "colalign", "nrmsize", "nrmalign",
             "possize", "posalign", "wtsize", "wtalign", "getVertexInfo",
             "GetIndexBounds", "StackEntry", "GeState", "state", "resetGeState",
             "push", "pop", "findFramebuf", "insertFramebuf", "AdvanceVerts", "patchGeList"]:
    part = re.sub(r"\b" + name + r"\b", "reference_" + name, part)
(root / "build-tools/gepatch_reference_slice.h").write_text(part)

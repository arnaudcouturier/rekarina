"""fontTools without its compiled modules, when REKARINA_FONTTOOLS_PURE=1: imported before the first fontTools import
(tools/button_icons.py, tools/make_ext_font.py).

fontTools ships a few of its modules twice, as Python source and compiled by Cython (fontTools.misc.bezierTools,
fontTools.cu2qu.cu2qu, ...), and Python takes the compiled one. The setup's prompts step runs its drawing tools again
with this switch after a crash (installer/oot.py Prompts.run): then the only native code in the process is Python's own.
The source modules do the same work, somewhat slower."""
import importlib.util
import os
import sys
from pathlib import Path

ON = os.environ.get('REKARINA_FONTTOOLS_PURE') == '1'


class _SourceFirst:
    """A finder ahead of the path finder for fontTools' modules: the .py beside a compiled module wins."""

    @staticmethod
    def find_spec(name, path=None, target=None):
        if not name.startswith('fontTools.') or not path:
            return None
        leaf = name.rpartition('.')[2]
        for folder in path:
            source = Path(folder) / f'{leaf}.py'
            compiled = any(p.suffix == '.pyd' or p.suffix == '.so' for p in Path(folder).glob(f'{leaf}.*'))
            if compiled and source.is_file():
                return importlib.util.spec_from_file_location(name, source)
        return None


if ON and not any(isinstance(f, _SourceFirst) for f in sys.meta_path):
    sys.meta_path.insert(0, _SourceFirst())

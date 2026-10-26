# Copyright 2026 Jetperch LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

from pyjls import repair
from .copy import _on_message, _on_progress


def parser_config(p):
    """Repair a damaged JLS file in place.  Back up the file first."""
    p.add_argument('path',
                   help='JLS file to repair, which must be writable')
    p.add_argument('--no-progress',
                   action='store_true',
                   help='Hide progress bar.')
    return on_cmd


def on_cmd(args):
    on_progress = None if args.no_progress else _on_progress
    repair(args.path, _on_message, on_progress)
    if on_progress is not None:
        print()
    return 0

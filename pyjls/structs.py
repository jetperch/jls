# Copyright 2021-2022 Jetperch LLC
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

from dataclasses import dataclass


@dataclass
class SourceDef:
    source_id: int
    name: str = None
    vendor: str = None
    model: str = None
    version: str = None
    serial_number: str = None

    def info(self, verbose=None) -> str:
        strs = [f'{self.source_id}: {self.name}']
        if verbose:
            for field in ['vendor', 'model', 'version', 'serial_number']:
                strs.append(f'    {field}: {getattr(self, field)}')
        return '\n'.join(strs)


@dataclass
class SignalDef:
    signal_id: int
    source_id: int
    signal_type: int = 0
    data_type: int = 0
    sample_rate: int = 0
    samples_per_data: int = 100000
    sample_decimate_factor: int = 1000
    entries_per_summary: int = 20000
    summary_decimate_factor: int = 100
    annotation_decimate_factor: int = 100
    utc_decimate_factor: int = 100
    name: str = None
    units: str = None
    length: int = 0

    def info(self, verbose=None) -> str:
        strs = [f'{self.signal_id}: {self.name}']
        if verbose:
            for field in ['source_id', 'signal_type', 'data_type', 'sample_rate',
                          'samples_per_data', 'sample_decimate_factor',
                          'entries_per_summary', 'summary_decimate_factor',
                          'annotation_decimate_factor', 'utc_decimate_factor',
                          'units', 'length']:
                strs.append(f'    {field}: {getattr(self, field)}')
        return '\n'.join(strs)

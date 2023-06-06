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
    """Define a source.

    Attributes:
        source_id       The source identifier.
        name            The source name string.
        vendor          The vendor string.
        model           The model string.
        version         The version string.
        serial_number   The serial number string.
    """
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
    """Define a signal.

    Attributes:
        signal_id                   The source identifier.
                                    0 reserved for global annotations, must be unique per instance.
        source_id                   The source identifier.
                                    Must match a SourceDef entry.
        signal_type                 The pyjls.SignalType for this signal.
        data_type                   The pyjls.DataType for this signal.
        sample_rate                 The sample rate per second (Hz).  0 for VSR.
        samples_per_data            The number of samples per data chunk.  (write suggestion)
        sample_decimate_factor      The number of samples per summary level 1 entry.
        entries_per_summary         The number of entries per summary chunk.  (write suggestion)
        summary_decimate_factor     The number of summaries per summary, level >= 2.
        annotation_decimate_factor  The annotation decimate factor for summaries.
        utc_decimate_factor         The UTC decimate factor for summaries.
        sample_id_offset            The sample id offset for the first sample.  (FSR only)
        name                        The signal name string.
        units                       The signal units string.
        length                      The length in samples.
    """
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
    sample_id_offset: int = 0
    name: str = None
    units: str = None
    length: int = 0

    def info(self, verbose=None) -> str:
        hdr = f'{self.signal_id}: {self.name}'
        if self.signal_type == 0:
            hdr += f' ({self.length} samples at {self.sample_rate} Hz)'
        strs = [hdr]
        if verbose:
            for field in ['source_id', 'signal_type', 'data_type', 'sample_rate',
                          'samples_per_data', 'sample_decimate_factor',
                          'entries_per_summary', 'summary_decimate_factor',
                          'annotation_decimate_factor', 'utc_decimate_factor',
                          'sample_id_offset',
                          'units', 'length']:
                strs.append(f'    {field}: {getattr(self, field)}')
        return '\n'.join(strs)

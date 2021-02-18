from dataclasses import dataclass

@dataclass
class SourceDef:
    source_id: int
    name: str = None
    vendor: str = None
    model: str = None
    version: str = None
    serial_number: str = None


@dataclass
class SignalDef:
    signal_id: int
    source_id: int
    signal_type: int = 0
    data_type: int = 0
    sample_rate: int = 0
    samples_per_data: int = 100000
    sample_decimate_factor: int = 100
    entries_per_summary: int = 20000
    summary_decimate_factor: int = 100
    annotation_decimate_factor: int = 100
    utc_decimate_factor: int = 100
    name: str = None
    si_units: str = None
    length: int = 0

# Contributing to LG Magic Remote Driver

## Development Setup

### Prerequisites

- **Linux** system (kernel 4.15+)
- **Kernel headers** for your running kernel
- **Python 3.9+**
- An **LG Magic Remote** (MR20 or similar) for hardware testing

### Kernel Module Development

```bash
# Install build tools
sudo apt-get install build-essential linux-headers-$(uname -r) dkms

# Build
cd kernel
make

# Load (requires root)
sudo insmod lg_magic.ko

# Check it loaded
dmesg | tail -20
lsmod | grep lg_magic

# Test with evtest
sudo evtest
```

### Python Tools Development

```bash
# Create virtual environment
python3 -m venv .venv
source .venv/bin/activate

# Install dependencies
pip install -r requirements-dev.txt

# Run tests
pytest tests/ -v

# Run linter
ruff check scripts/ tests/
ruff format scripts/ tests/
```

### DKMS Installation (for testing)

```bash
sudo mkdir -p /usr/src/lg-magic-1.0
sudo cp kernel/* /usr/src/lg-magic-1.0/
sudo dkms add -m lg-magic -v 1.0
sudo dkms build -m lg-magic -v 1.0
sudo dkms install -m lg-magic -v 1.0
```

## Coding Style

### C (Kernel Module)
- Follow the [Linux kernel coding style](https://www.kernel.org/doc/html/latest/process/coding-style.html)
- Use `checkpatch.pl --strict` before committing
- No floating-point in kernel code (use fixed-point math)
- Keep lines under 100 columns where practical

### Python
- Follow [PEP 8](https://peps.python.org/pep-0008/)
- Use `ruff` for linting and formatting
- Type hints encouraged but not required
- Use `pathlib` for file paths, `tempfile` for temporary files in tests

## Testing

### Running Tests

```bash
# All tests
pytest tests/ -v

# With coverage
pytest tests/ -v --cov=scripts --cov-report=html

# Specific test file
pytest tests/test_calibrate.py -v

# Integration test (requires scipy)
bash tests/integration/calib_roundtrip.sh
```

### Writing Tests
- Place unit tests in `tests/test_<module>.py`
- Use `pytest` fixtures where appropriate
- Mock hardware-dependent imports (evdev, uinput) in unit tests
- Integration tests go in `tests/integration/`

## Branch Strategy

- `master` — stable releases only
- `develop` — active development, merge into master for releases
- Feature branches — `feature/<name>` from develop, merge back via PR

## Release Process

1. Ensure all tests pass on `develop`
2. Update version in `pyproject.toml` and `dkms.conf`
3. Update `CHANGELOG.md`
4. Merge to `master`
5. Tag: `git tag -a vX.Y.Z -m "vX.Y.Z"`
6. Push tag: `git push origin vX.Y.Z`
7. GitHub Actions builds and publishes release artifacts

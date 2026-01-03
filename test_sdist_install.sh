#!/bin/bash
# Test script to verify sdist installation works locally
# This simulates the test-sdist GitHub Actions job

set -e  # Exit on error

echo "=== IOWarp Core sdist Installation Test ==="
echo ""

# Check if conda is available
if ! command -v conda &> /dev/null; then
    echo "ERROR: conda is not installed."
    echo ""
    echo "To install Miniconda:"
    echo "  wget https://repo.anaconda.com/miniconda/Miniconda3-latest-Linux-x86_64.sh"
    echo "  bash Miniconda3-latest-Linux-x86_64.sh -b -p \$HOME/miniconda3"
    echo "  source \$HOME/miniconda3/bin/activate"
    echo ""
    echo "Then run this script again."
    exit 1
fi

# Create a clean test environment
ENV_NAME="iowarp_sdist_test"
echo "=== Removing old test environment if it exists ==="
conda remove -n $ENV_NAME --all -y 2>/dev/null || true

echo ""
echo "=== Creating clean conda environment: $ENV_NAME ==="
conda create -n $ENV_NAME python=3.10 -y

# Activate the environment
echo ""
echo "=== Activating conda environment ==="
eval "$(conda shell.bash hook)"
conda activate $ENV_NAME

# Verify conda is active
echo "Active environment: $CONDA_DEFAULT_ENV"
echo "Python: $(which python)"

# Install conda dependencies (matching GitHub Actions workflow)
echo ""
echo "=== Installing conda dependencies ==="
conda install -y \
  conda \
  conda-build \
  cereal \
  catch2 \
  cmake \
  boost \
  yaml-cpp \
  zeromq \
  cxx-compiler \
  pkg-config \
  openmpi \
  elfutils \
  zlib \
  h5py

# Verify conda and conda-build are installed
echo ""
echo "=== Verifying conda installation ==="
conda --version
conda-build --version
python -c "import conda; print('conda module version:', conda.__version__)"

# Set up MPI environment
echo ""
echo "=== Setting up MPI environment ==="
export MPI_HOME=$CONDA_PREFIX
export MPI_C_COMPILER=$CONDA_PREFIX/bin/mpicc
export MPI_CXX_COMPILER=$CONDA_PREFIX/bin/mpic++
echo "MPI_HOME=$MPI_HOME"
echo "MPI_C_COMPILER=$MPI_C_COMPILER"

# Install build dependencies (matching GitHub Actions workflow)
echo ""
echo "=== Installing Python build dependencies ==="
python -m pip install --upgrade pip setuptools wheel nanobind setuptools-scm

# Set up conda environment for build (matching GitHub Actions workflow)
echo ""
echo "=== Setting up conda environment for build ==="
PY_VERSION=$(python -c "import sys; print(f'{sys.version_info.major}.{sys.version_info.minor}')")
echo "Python version: $PY_VERSION"

# Ensure conda module is in PYTHONPATH for conda-build
CONDA_SITE_PACKAGES="$CONDA_PREFIX/lib/python${PY_VERSION}/site-packages"
export PYTHONPATH="$CONDA_SITE_PACKAGES:$PYTHONPATH"
echo "PYTHONPATH=$PYTHONPATH"

# Verify conda module is importable
echo ""
echo "=== Verifying conda module is importable ==="
python -c "import sys; print('Python path:', sys.path)"
python -c "import conda; print('✓ conda module found:', conda.__version__)"

# Build source distribution
echo ""
echo "=== Building source distribution ==="
cd /home/hyoklee/core
python -m build --sdist

# Verify sdist was created
echo ""
echo "=== Verifying source distribution ==="
ls -lh dist/*.tar.gz
SDIST_FILE=$(ls -t dist/*.tar.gz | head -1)
echo "Latest sdist: $SDIST_FILE"

# Test installation from sdist
echo ""
echo "=== Installing from source distribution ==="
pip install "$SDIST_FILE" --verbose

# Test the installation
echo ""
echo "=== Testing installation ==="
python -c "import iowarp_core; print('✓ Installed version:', iowarp_core.get_version())"
python -c "import sys; print('✓ Python executable:', sys.executable)"

# Verify conda is still available
echo ""
echo "=== Final verification ==="
conda --version
python -c "import conda; print('✓ conda module version:', conda.__version__)"

echo ""
echo "=== ✓ Test completed successfully! ==="
echo ""
echo "To clean up, run:"
echo "  conda deactivate"
echo "  conda remove -n $ENV_NAME --all -y"

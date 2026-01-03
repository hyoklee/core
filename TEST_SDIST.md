# Testing sdist Installation Fix

## Changes Made to `.github/workflows/build-dist.yml`

The `test-sdist` job has been updated to fix conda dependency errors:

1. **Added build dependencies step**:
   - Installs `pip`, `setuptools`, `wheel`, `nanobind`, `setuptools-scm` using `python -m pip`
   - Ensures all build tools are available before installing the sdist

2. **Added PYTHONPATH configuration step** (NEW - fixes the conda module import error):
   - Detects Python version dynamically
   - Sets `PYTHONPATH` to include conda site-packages directory
   - Ensures the conda Python module is importable by conda-build
   - Verifies conda module can be imported before proceeding

3. **Fixed installation command**:
   - Changed from `$CONDA_PREFIX/bin/pip install dist/*.tar.gz`
   - To `pip install dist/*.tar.gz`
   - Uses pip within the activated conda environment (via `shell: bash -l {0}`)

4. **Conda dependencies already included**:
   - `conda` and `conda-build` are already installed in the conda dependencies step
   - These are required for the build process

## Why This Fixes the Error

The original error occurred because:
- The `install.sh` script called by `setup.py` executes `conda build`
- The `conda` command tries to import the conda Python module: `from conda.cli import main`
- Even though conda and conda-build were installed via `conda install`, the conda Python module was not in PYTHONPATH
- This caused `ModuleNotFoundError: No module named 'conda'`

The fix ensures:
- `PYTHONPATH` explicitly includes the conda site-packages directory before build
- The conda Python module is importable by the `conda` and `conda-build` commands
- Verification step confirms conda module can be imported before proceeding
- All commands run within the activated conda environment (`shell: bash -l {0}`)
- All dependencies (both conda and pip) are properly resolved

## Testing Locally

### Prerequisites
You need conda installed. If not:
```bash
wget https://repo.anaconda.com/miniconda/Miniconda3-latest-Linux-x86_64.sh
bash Miniconda3-latest-Linux-x86_64.sh -b -p $HOME/miniconda3
source $HOME/miniconda3/bin/activate
```

### Run the Test Script
```bash
cd /home/hyoklee/core
./test_sdist_install.sh
```

### What the Test Script Does
1. Creates a clean conda environment (`iowarp_sdist_test`)
2. Installs all conda dependencies (including conda and conda-build)
3. Verifies conda module is available
4. Installs Python build dependencies
5. Builds the source distribution
6. Installs from the sdist
7. Tests the installation
8. Verifies conda is still available after installation

### Manual Testing Steps
If you prefer to test manually:
```bash
# 1. Create and activate conda environment
conda create -n test_sdist python=3.10 -y
conda activate test_sdist

# 2. Install conda dependencies
conda install -y conda conda-build cereal catch2 cmake boost yaml-cpp \
  zeromq cxx-compiler pkg-config openmpi elfutils zlib h5py

# 3. Verify conda module
python -c "import conda; print('conda version:', conda.__version__)"

# 4. Install build dependencies
python -m pip install --upgrade pip setuptools wheel nanobind setuptools-scm

# 5. Set up PYTHONPATH for conda-build
PY_VERSION=$(python -c "import sys; print(f'{sys.version_info.major}.{sys.version_info.minor}')")
export PYTHONPATH="$CONDA_PREFIX/lib/python${PY_VERSION}/site-packages:$PYTHONPATH"

# 6. Verify conda module is importable
python -c "import conda; print('conda module version:', conda.__version__)"

# 7. Build and install sdist
cd /home/hyoklee/core
python -m build --sdist
pip install dist/*.tar.gz --verbose

# 8. Test installation
python -c "import iowarp_core; print('Version:', iowarp_core.get_version())"

# 9. Cleanup
conda deactivate
conda remove -n test_sdist --all -y
```

## Expected Results

### Success Indicators
- ✓ conda module imports successfully before and after pip install
- ✓ sdist builds without errors
- ✓ sdist installs without conda dependency errors
- ✓ `iowarp_core` imports successfully after installation
- ✓ Version is displayed correctly

### Failure Indicators
- ✗ `ModuleNotFoundError: No module named 'conda'`
- ✗ Build fails with dependency errors
- ✗ Installation fails with library not found errors

## GitHub Actions Testing

The changes are already in `.github/workflows/build-dist.yml`. To test in GitHub Actions:

1. **Option 1: Push to a branch and trigger workflow**
   ```bash
   git add .github/workflows/build-dist.yml
   git commit -m "fix: resolve conda dependency error in test-sdist job"
   git push origin your-branch
   # Manually trigger workflow from GitHub Actions UI
   ```

2. **Option 2: Create a test tag**
   ```bash
   git tag -a v0.0.1-test -m "Test sdist installation fix"
   git push origin v0.0.1-test
   # This will automatically trigger the workflow
   ```

3. **Option 3: Workflow dispatch**
   - Go to GitHub Actions tab
   - Select "Build Distribution" workflow
   - Click "Run workflow"
   - Select your branch
   - Click "Run workflow"

## Troubleshooting

### If conda module is still not found:
1. Verify conda and conda-build are in the environment:
   ```bash
   conda list | grep conda
   ```

2. Check Python can import conda:
   ```bash
   python -c "import sys; print(sys.path)"
   python -c "import conda; print(conda.__file__)"
   ```

3. Ensure PYTHONPATH includes conda site-packages:
   ```bash
   PY_VERSION=$(python -c "import sys; print(f'{sys.version_info.major}.{sys.version_info.minor}')")
   export PYTHONPATH="$CONDA_PREFIX/lib/python${PY_VERSION}/site-packages:$PYTHONPATH"
   echo "PYTHONPATH=$PYTHONPATH"
   python -c "import conda; print('Success!')"
   ```

4. Ensure you're using pip from the conda environment:
   ```bash
   which pip
   # Should show: /path/to/conda/envs/test_sdist/bin/pip
   ```

### If build still fails:
- Check build dependencies are installed: `pip list | grep -E "setuptools|wheel|nanobind"`
- Verify CMake can find conda packages: `cmake --system-information | grep -i conda`
- Check the verbose build output for specific missing dependencies

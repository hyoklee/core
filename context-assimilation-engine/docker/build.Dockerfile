# Dockerfile for building the Content Assimilation Engine (CAE)
# CAE depends on CTE (Content Transfer Engine), so we inherit from the CTE build image
# which contains CTE and all its dependencies (Chimaera runtime, etc.)

FROM iowarp/context-transfer-engine-build:latest

# Set working directory
WORKDIR /workspace

# Copy the entire CAE source tree
COPY . /workspace/

# Initialize git submodules and build
# Install to both /usr/local and /iowarp-cae for flexibility
RUN sudo chown -R $(whoami):$(whoami) /workspace && \
    git submodule update --init --recursive && \
    mkdir -p build && \
    cmake --preset release && \
    cmake --build build -j$(nproc) && \
    sudo cmake --install build --prefix /usr/local && \
    sudo cmake --install build --prefix /iowarp-cae && \
    sudo rm -rf /workspace


# Add iowarp-cae to Spack configuration
RUN echo "  iowarp-cae:" >> ~/.spack/packages.yaml && \
    echo "    externals:" >> ~/.spack/packages.yaml && \
    echo "    - spec: iowarp-cae@main" >> ~/.spack/packages.yaml && \
    echo "      prefix: /usr/local" >> ~/.spack/packages.yaml && \
    echo "    buildable: false" >> ~/.spack/packages.yaml

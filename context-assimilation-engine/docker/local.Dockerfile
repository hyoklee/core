# Dockerfile for building Content Assimilation Engine (CAE) from local build
# CAE depends on CTE (Content Transfer Engine), so we inherit from the CTE build image
# Expects that the CAE source has already been configured and built locally

FROM iowarp/context-transfer-engine-build:latest

COPY . /workspace

WORKDIR /workspace

RUN cd build && sudo make -j$(nproc) install
RUN sudo rm -rf /workspace

# Add iowarp-cae to Spack configuration
RUN echo "  iowarp-cae:" >> ~/.spack/packages.yaml && \
    echo "    externals:" >> ~/.spack/packages.yaml && \
    echo "    - spec: iowarp-cae@main" >> ~/.spack/packages.yaml && \
    echo "      prefix: /usr/local" >> ~/.spack/packages.yaml && \
    echo "    buildable: false" >> ~/.spack/packages.yaml

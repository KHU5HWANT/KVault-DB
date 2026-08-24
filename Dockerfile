FROM ubuntu:22.04

# Avoid tzdata interactive prompt during package installation
ENV DEBIAN_FRONTEND=noninteractive

# Install build dependencies
RUN apt-get update && apt-get install -y \
    g++-13 \
    cmake \
    ninja-build \
    git \
    libssl-dev \
    && rm -rf /var/lib/apt/lists/*

# Set g++-13 as the default compiler
RUN update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-13 13 \
    && update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-13 13

# Set the working directory
WORKDIR /app

# Copy the project files
COPY . .

# Configure and Build the project
RUN cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
RUN cmake --build build --parallel

# Render expects web services to listen on port 8080 by default (or via PORT env var)
EXPOSE 8080

# Ensure data directories exist
RUN mkdir -p data/sstables data/wal

# Set the entrypoint to the compiled C++ binary
CMD ["./build/kvault_server"]

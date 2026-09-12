FROM ubuntu:24.04
# Cache-bust: 2026-09-09-v3 (before_handle OPTIONS fix + .env.production)

# Avoid tzdata interactive prompt during package installation
ENV DEBIAN_FRONTEND=noninteractive

# Install build dependencies (Ubuntu 24.04 natively comes with gcc-13 as default)
RUN apt-get update && apt-get install -y \
    g++ \
    gcc \
    cmake \
    ninja-build \
    git \
    libssl-dev \
    python3 \
    && rm -rf /var/lib/apt/lists/*

# Force GCC to aggressively garbage collect to keep RAM usage under 512MB
ENV CXXFLAGS="-Os -g0 --param ggc-min-expand=1 --param ggc-min-heapsize=32768"

# Set the working directory
WORKDIR /app

# Copy the project files
COPY . .

# Configure and Build the project (MinSizeRel uses -Os which drastically reduces compiler RAM usage compared to Release -O3)
RUN cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=MinSizeRel -DBUILD_TESTING=OFF
# Run strictly sequential build (-j 1) to prevent Render's 512MB RAM limit from triggering the OOM killer
RUN cmake --build build -j 1 --target kvault_server

# Render expects web services to listen on port 8080 by default (or via PORT env var)
EXPOSE 8080

# Ensure data directories exist
RUN mkdir -p data/sstables data/wal

# Set the entrypoint to the compiled C++ binary
CMD ["./build/kvault_server"]

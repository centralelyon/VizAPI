FROM python:3.13.13-trixie

# Add metadata labels to the image
LABEL org.opencontainers.image.source=https://github.com/centralelyon/VizAPI
LABEL org.opencontainers.image.description="VizAPI docker image."
LABEL org.opencontainers.image.licenses=MIT


RUN apt-get update && apt-get install -y \
    && rm -rf /var/lib/apt/lists/*
    
WORKDIR /vizapi

COPY requirements.txt .

RUN pip install --no-cache-dir -r requirements.txt

COPY pyproject.toml .
COPY public public/
COPY app app/

# Copy Cities data - by Mu
COPY data/modules/decaligne_code/cities/ \
     /vizapi/decaligne-seed-data/cities/

# Build Decaligne LOOM / octi - by Mu
RUN chmod +x app/modules/decaligne_code/loom/build.sh \
    && app/modules/decaligne_code/loom/build.sh

# Build Decaligne TransitMap C++ Core - by Mu
RUN apt-get update && apt-get install -y \
    g++ \
    cmake \
    make \
    && rm -rf /var/lib/apt/lists/*

RUN cmake \
    -S app/modules/decaligne_code/transitmap_core \
    -B /tmp/transitmap-core-build \
    -DTRANSIT_BUILD_DESKTOP=OFF \
    -DBUILD_TESTING=OFF \
    -DCMAKE_BUILD_TYPE=Release \
    && cmake --build /tmp/transitmap-core-build \
    --target transitmap-core \
    -j2 \
    && cp /tmp/transitmap-core-build/transitmap-core /usr/local/bin/transitmap-core \
    && rm -rf /tmp/transitmap-core-build

ENV TRANSITMAP_CORE_BIN=/usr/local/bin/transitmap-core

RUN sed -i 's/\r$//' /vizapi/app/modules/decaligne_code/init_data.sh \
    && chmod +x /vizapi/app/modules/decaligne_code/init_data.sh

# this is wrong
# COPY data data/ 

EXPOSE 8000

ENTRYPOINT ["sh", "-c", "fastapi run --host 0.0.0.0 --port 8000"]

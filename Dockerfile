FROM python:3.13.13-trixie

RUN apt-get update && apt-get install -y \
    && rm -rf /var/lib/apt/lists/*
    
WORKDIR /vizapi

COPY requirements.txt .

RUN pip install --no-cache-dir -r requirements.txt

COPY pyproject.toml .
COPY app app/

# this is wrong
COPY data data/ 

EXPOSE 8000

ENTRYPOINT ["sh", "-c", "fastapi run --host 0.0.0.0 --port 8000"]
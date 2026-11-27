FROM docker.io/library/python:3.13-slim
WORKDIR /app
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt
COPY ingest.py .
# Ausgabe sofort ins Log von docker compose
ENV PYTHONUNBUFFERED=1
USER nobody
CMD ["python", "ingest.py"]

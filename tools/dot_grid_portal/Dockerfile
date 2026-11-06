FROM python:3.14-slim-bookworm AS dwg-build
RUN sed -i s,http://deb.debian.org,https://deb.debian.org,g /etc/apt/sources.list.d/debian.sources \
 && apt-get -o Acquire::https::Timeout=20 update && apt-get install -y --no-install-recommends build-essential pkg-config curl ca-certificates xz-utils && rm -rf /var/lib/apt/lists/*
WORKDIR /build
RUN curl -fLsS https://ftp.gnu.org/gnu/libredwg/libredwg-0.14.tar.xz -o libredwg.tar.xz \
 && echo '62ebb73b984f865960f20ed26619ea5f8789d5e3fd088fa40a2598384da81275  libredwg.tar.xz' | sha256sum -c - \
 && tar -xf libredwg.tar.xz \
 && cd libredwg-0.14 && ./configure --disable-shared --disable-bindings --disable-docs \
 && make -j2 && make install
FROM python:3.14-slim-bookworm
COPY --from=dwg-build /usr/local/bin/dwg2dxf /usr/local/bin/dwg2dxf
COPY --from=dwg-build /build/libredwg-0.14/test/test-data/2000/Line.dwg /opt/cad-test-fixtures/Line.dwg
COPY --from=dwg-build /build/libredwg-0.14/COPYING /usr/share/doc/libredwg/COPYING
WORKDIR /app
COPY tools/dot_grid_portal/generator/requirements.lock.txt /app/requirements.lock.txt
RUN pip install --no-cache-dir -r requirements.lock.txt && useradd --uid 10001 --create-home cad
COPY scripts/dot_grid /app/scripts/dot_grid
COPY resources/defaults/dot_grid /app/resources/defaults/dot_grid
COPY tools/dot_grid_portal/generator /app/tools/dot_grid_portal/generator
USER cad
ENV OPENBLAS_NUM_THREADS=1 OMP_NUM_THREADS=1
EXPOSE 8080
CMD ["python","-m","uvicorn","tools.dot_grid_portal.generator.server:app","--host","0.0.0.0","--port","8080","--workers","1"]

# Revisión de consumo de memoria de codebase-memory-mcp

Fecha: 2026-09-10.

## Resultado

No hay evidencia de una fuga de RAM en el estado observado. La captura del host mostró 8 procesos CBM: un daemon permanente y 7 frontends stdio. El daemon tenía aproximadamente 12 MiB RSS y 17–18 MiB de footprint físico; los frontends sumaban aproximadamente 16 MiB RSS. El host tenía 77 % de memoria libre y no registraba swap-in ni swap-out.

El VSZ del daemon, aproximadamente 416 GiB, es espacio de direcciones virtuales reservado por macOS. No debe sumarse como memoria física.

La medición de memoria del host mostró aproximadamente 11 GiB usados, pero CBM no era el origen: el proceso más grande era OrbStack (~2,3 GiB), seguido por `mds_stores`/Spotlight (~0,9 GiB), renderizadores de VS Code y Chrome, Orca y los propios procesos Codex/Claude. El agregado RSS de CBM era aproximadamente 28 MiB. Las sumas por proceso pueden contar memoria compartida más de una vez, por lo que sirven para localizar responsables, no como contabilidad exacta del total.

Sí existe un pico confirmado durante indexación: un worker de `CO Refactor Project` (39.291 archivos, 750.143 nodos) registró `rss_mb=10.999`, `peak_mb=13.151`, `budget_mb=12.288` y fue terminado mientras comenzaba la fase `parallel.resolve`. El daemon registró posteriormente `worker_cancelled` y `watcher.index.retry` para ese proyecto. Esto explica la observación de aproximadamente 11 GB: es un worker de indexación grande, no el daemon idle.

La misma ejecución registró `parallel.extract.start ... workers=18`, aunque el LaunchAgent del daemon declaraba `CBM_WORKERS=4`. La causa quedó confirmada: el daemon se detuvo al desconectarse su último cliente y el siguiente cliente levantó un daemon temporal con su propio entorno, que no incluía la variable del LaunchAgent; al faltar `CBM_WORKERS`, el indexador usó los 18 cores detectados. No era un cálculo erróneo de 4 a 18.

El problema material es el tamaño del caché persistente y su actividad de I/O: `~/.cache/codebase-memory-mcp` ocupa aproximadamente 18,4 GB, contiene 101 bases SQLite y 41 archivos `.stage*` que suman aproximadamente 5,4 GB. Los stages no aparecían abiertos en la captura de `lsof` y los más grandes datan del 29–30 de agosto; parecen residuos de indexaciones interrumpidas, pero no se eliminaron porque borrar caché es una operación destructiva y no se autorizó.

## Presión potencial durante indexación

La configuración efectiva es `auto_index=false`, `auto_watch=true`, `watcher_enabled=true`, `ui_enabled=true` y `max_index_jobs=2`. Los logs del daemon registran un presupuesto lógico de 24.576 MiB sobre 49.152 MiB de RAM, es decir, 50 % del host; con dos jobs son hasta 12.288 MiB por worker. Ese presupuesto limita indexaciones activas, no el RSS del daemon en reposo.

La extracción paralela puede retener hasta 1 GiB de fuentes por ejecución por defecto y cada archivo retenido está limitado a 32 MiB. SQLite solicita mmap de 64 MiB por conexión y usa `temp_store=MEMORY`; mmap aumenta el espacio virtual y solo las páginas tocadas pasan a residencia física. La fórmula de `cbm_mem_ram_fraction_for_total` usa 35 % cuando la máquina tiene hasta 32 GiB, por lo que el presupuesto lógico de una máquina de 32 GiB es aproximadamente 11,2 GiB.

## Recomendación operativa

Para contener el incidente se detuvieron los workers exactos que estaban consumiendo 34,32 GB, se desactivó el watcher y se recargó el LaunchAgent del daemon con `CBM_MEM_BUDGET_MB=4096` y `CBM_WORKERS=1`. El daemon PID 67131 arrancó con `memory_budget_bytes=4294967296`, `physical_job_limit=2`, `worker_memory_budget_bytes=2147483648` y `watcher.disabled`; no hay workers activos ni clientes comprometidos. Esta mitigación evita nuevos reintentos automáticos y reduce el presupuesto lógico por worker a 2 GiB, a cambio de indexaciones manuales más lentas.

No se debe volver a ejecutar el indexado completo de `CO Refactor Project` en el Mac para validar el cambio: la directiva de cómputo exige ejecutar pruebas y cargas pesadas en infraestructura Quantum remota. El parche de código fue compilado y probado en `qf-infra` (Linux, suite `pipeline`: 274/274), pero aún requiere un build arm64 macOS antes de instalarse en el host.

Antes de una limpieza, conservar backup y comprobar que ningún stage esté abierto. Si el uso de consultas lo permite, `CBM_SQLITE_MMAP_SIZE=0` puede reducir residencia de páginas; debe medirse junto al canary y no se cambió automáticamente.

La limpieza de los 5,4 GB de stages debe implementarse como una operación explícita y segura (detección de edad, exclusión de archivos abiertos, backup o cuarentena), no como un `rm` indiscriminado del caché. Estos archivos son principalmente presión de disco y de I/O; la captura no demuestra que estén ocupando 5,4 GB de RAM.

El watcher puede volver a encolar el mismo repositorio después de un cambio o una cancelación; en las métricas recientes hubo varias ejecuciones de `CO-Refactor-Project`, `Second-Brain` y `quantum-chief-of-staff`. No había un worker activo en la captura final, pero la secuencia de retry confirma churn de indexación.

## Evidencia y límites

Las mediciones se realizaron con `ps`, `vmmap`, `footprint`, `lsof`, `memory_pressure`, `vm_stat`, `du` y consultas SQLite de solo lectura. El worker observado fue `/Users/gggp/.local/bin/codebase-memory-mcp`, PID 68667, build `7f0a14f37b2d…`; se terminó ese PID exacto. Después se recargó el LaunchAgent y el daemon quedó en PID 95782 con el presupuesto de 4 GiB. La captura es un punto en el tiempo y no sustituye una serie temporal durante una indexación grande.

## Nota de retiro del guard de resolve (2026-09-27)

El parche citado más arriba («El parche de código fue compilado y probado en `qf-infra`») incluía un guard de memoria en `resolve_worker` (`src/pipeline/pass_parallel.c`, `1356220d`, portado a la memoria core y al fail-whole nombrado en `bc12624b`): sondeaba el presupuesto cada 8 archivos por worker y, con la máquina o el proceso sobre presupuesto, abortaba la fase `parallel.resolve` completa — se descartaba la salida parcial sin fusionar y la ejecución devolvía `CBM_PIPELINE_ABORT_OVER_BUDGET`, que MCP nombra `over_memory_budget`, con el registro `parallel.resolve.memory_budget_exceeded`.

Ese guard se retira en un commit aparte según el criterio del ADR §A3: si en A7 tres corridas del corpus de referencia con el presupuesto operativo nunca lo disparaban, sobraba. La evidencia (informe A7, binario integrado sobre el corpus real `CO Refactor Project` de 104.234 archivos, `CBM_MEM_BUDGET_MB=6144 CBM_WORKERS=4`): 3/3 corridas completas en 421–438 s con 314.225 nodos / 2.063.124 aristas, pico de fase `parallel_resolve` de 4.899–4.912 MB (80 % del presupuesto) y **0** disparos de `parallel.resolve.memory_budget_exceeded`. El binario anterior muere por OOM del cgroup en las 3 corridas a ≥11.942 MB, con solo ~34 % de la extracción hecha.

Se conserva el resto de la red de seguridad: spill de extracción (`mem.spill.*`), fail-whole de extracción (`mem.budget.exceeded` → `CBM_PIPELINE_ABORT_OVER_BUDGET` → `over_memory_budget`), y el tope operativo `index_max_files=45000`. Riesgo residual: la ausencia de disparos cubre el corpus de referencia (carga ≤ 4,72 GB); para repos mucho mayores, una fase `parallel.resolve` sobre presupuesto completa y publica en vez de abortar, y lo contienen esas tres piezas. El contrato por fases queda fijado en `tests/test_pipeline.c` (`pipeline_extract_over_budget_spills_and_completes`, `pipeline_over_budget_resolve_completes_without_guard`).

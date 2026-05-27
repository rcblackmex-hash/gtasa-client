# HITO 3 - Render de jugadores remotos

## Objetivo

Cuando un amigo se conecta al server, su CJ aparece renderizado en la
pantalla del GTA SA del jugador local, posicionado en las coords del
broadcast `PLAYERS|...`.

## Versionado

### v0.12-stable (HITO 2 OK)
- TCP + heartbeat 5s + lectura POS real via `Players[i] -> +0x18 -> +0x30`
- ESTABLE 4+ minutos sin crash

### v0.14-deepdump
- Descubrimiento: la struct en `Players[i]+0x18` es un CMatrix*, no CPed*.
- `Players[i]` ES el CPed mismo.

### v0.15-vtable
- Vtable CPlayerPed identificada @ base+0x008330f8
- Destructor @ base+0x005c0efc

### v0.16-ctorscan
- Pattern scan para constructor: 1 hit (el dtor conocido).
- Constructor probablemente inlineado.

### v0.17/v0.18/v0.19-safescan
- Heap scan con signal handler SIGSEGV/SIGBUS + sigsetjmp
- 9 vtables distintas, 703 candidatos, 543 regiones OK
- CPlayerPed confirmado (1 instancia = CJ)

### v0.20-hijacktest
- Sobreescribir CMatrix+0x30 de 4 samples (top vtables)
- CONFIRMADO VISUALMENTE: aparecio un COCHE al lado del CJ
- Todos "POS PERSISTE" = vtables sin AI propia
- Ninguno crasheo. Hijack es safe.

### v0.21-aitest
- Hijack + muestreo cada 1s durante 6s para detectar AI
- RESULTADO: vtable `base+0x0083bb50` (10 inst) mostro MOVING smooth
  ~1m/s (drift 8.8 -> 10.4m en 6s). Confirmado VISUAL: peatona en bikini
  aparecio a la derecha del CJ y camino.
- CPed REAL CONFIRMADO: `base+0x0083bb50`
- Otras vtables: 0x00824eb0 (520 inst, CObject), 0x008303b0 (147 inst,
  probable CDummyObject), 0x0082ffc8 (1 inst, MOVING erratico = CPhysical
  dormido despertando con physics).

### v0.22-pedlock (ACTUAL)
- Scan dirigido `ScanCPedPool()`: solo guarda objetos con vtable
  == base+0x0083bb50. Devuelve hasta 64 samples ordenados por dist al CJ.
- Pool `g_CPedSamples` con `assignedRemoteId` (-1 = libre).
- Thread `PedLockThreadFn` corriendo cada 100ms:
  * libera samples cuyo remoto ya no esta
  * asigna remotos sin sample al primer sample libre
  * sobreescribe `CMatrix+0x30` del sample con la pos del amigo
  * `sigsetjmp` proteccion: si el ped fue freed entre rescan y write,
    se marca el sample como inutilizable y se sigue
- Re-scan automatico cada 60s para refrescar pool (CPeds se streamean
  cuando el CJ camina)
- Modo TestBro: si en 30s no hay otros conectados, se crea un remoto fake
  id=999 orbitando alrededor del CJ a 4m de radio, periodo 8s. Eso le
  permite al user solitario ver el pedlock funcionando.

### v0.23-pedlock-clean (ACTUAL)
- Filtro anti-vehiculo: antes del scan CPed se hace mini-scan de
  vehiculos (vtable 0x00831ba8 confirmada en v0.20). Para cada CPed
  candidato, si su pos esta a <3m de algun vehiculo, lo skippeamos.
  Solo se hijackean peatones LIBRES caminando.
- TestBro update movido al LockThread (cada 100ms en vez de cuando
  llegue net traffic) -> orbital smooth.
- Sincronizacion sampleIdx en g_Remotes -> state log limpio.

### v0.24-pedlock-validate (ACTUAL)
- Problema detectado en v0.23: aunque el filtro espacial dejaba pasar
  peatones a >3m de vehiculos, algunos peds tenian un "tether" interno
  al coche (sentados hace 1s, task de entrar/salir, animacion scripted).
  El game arrastraba el coche al ped hijackeado.
- Solucion: detectar tether en runtime. Entre tick y tick (100ms) leemos
  la pos actual del sample y la comparamos con la pos que ESCRIBIMOS el
  tick anterior. Si difiere mas de 3m sin nuestra intervencion -> tether.
- 2 strikes consecutivos = sample marcado `bad`, descartado del pool,
  lock auto-rota al siguiente sample libre.
- Threshold espacial CPed-vehicle subido de 3m a 5m (mas conservador).
- Rescan adaptativo: 60s normal, 10s si pool agotado, 20s si pool vacio.

### v0.25-pedhover (ACTUAL)
- Problema en v0.24: self-healing rota samples y elimina los que tienen
  tether visible por drift entre ticks. Sample final estable (4.0m,
  obedece). PERO user sigue viendo coche junto al CJ.
- Hipotesis: el peaton hijackeado tiene MODELO no cargado por streaming
  (originalmente era de otra zona). Es INVISIBLE. Lo que se ve es
  trafico normal del juego que el user asocia a nuestro peaton.
- Test inconfundible v0.25: TestBro deja de orbital, ahora va FIJO a
  CJ + (0,0,+8m). Con bounce vertical +6..+10m cada 2s. Si funciona el
  user vera peaton FLOTANDO sobre su cabeza. Los coches no flotan =
  inconfundible.
- Threshold tether 3.0m -> 1.5m (mas estricto).
- Dump del primer sample asignado: primeros 0x800 bytes anotados con
  HEAP / CODE base+X / VEHICLE_REF (qword que apunte a addr de un
  vehiculo del scan). Esto identifica m_pVehicle offset.

### v0.26-identify (ACTUAL)
- HALLAZGO CRITICO v0.25: el TestBro hover a +8m del CJ aparecio como
  VEHICULO COMPLETO flotando, no un peaton. Dump confirmo vehicle_ref_hits=0.
  Conclusion: la vtable 0x0083bb50 que usabamos NO es CPed, es un tipo
  de VEHICULO. La "peatona en bikini" de v0.21 fue coincidencia visual.
- Plan v0.26: vtable farm para reidentificar CPed.
  * Histograma completo de vtables en el heap.
  * Top 6 candidatas (count>=5, minDist<35m), EXCLUYENDO conocidas:
    0x008330f8 (CPlayerPed), 0x00831ba8 (coche v0.20), 0x0083bb50
    (vehiculo falso-CPed v0.25).
  * Para cada candidata: 1 sample posicionado FIJO en cuadricula 2x3
    arriba del CJ a +6m altura.
  * Sostener pos cada 200ms durante 10s.
  * User mira arriba y reporta cual de los 6 se ve como PERSONA.
  * Esa vtable = CPed real -> v0.27 pedlock real.

### v0.27-pedlock-real (proxima)
- Una vez identificada la vtable real de CPed por el user, volver al
  flujo pedlock de v0.24 (self-healing, threshold tether 1.5m) pero
  con la vtable correcta.
- Para cada amigo remoto en g_Remotes:
  - Tomar 1 CPed sample no usado (de la vtable identificada como CPed)
  - Loop continuo (cada 50ms) reescribiendo CMatrix+0x30 con pos del amigo
  - Override-AI on demand: probar offset de flag "freeze" / health 0
- Resultado: amigos como NPCs en sus pos reales = primera version JUGABLE

## Patron del CMatrix (confirmado desde v0.13)

```
CMatrix layout:
  +0x00..+0x0F : right vector
  +0x10..+0x1F : forward vector
  +0x20..+0x2F : up vector
  +0x30..+0x3F : position (X, Y, Z, _)
  +0x50..+0x57 : owner ptr (backref a CPed dueno)
```

## Vtables descubiertas (v0.19 histogram)

| vtable rel       | inst | minDist | hipotesis           |
|------------------|------|---------|---------------------|
| base+0x00824eb0  | 444  | 4.5m    | CObject? (props)    |
| base+0x008303b0  | 155  | 9.9m    | candidate           |
| base+0x00830218  |  80  | 12.5m   | CANDIDATE FUERTE CPed |
| base+0x00831ba8  |  12  | 9.0m    | coche (v0.20 lo confirmo visual) |
| base+0x008330f8  |   1  | 0m      | CPlayerPed = CJ     |

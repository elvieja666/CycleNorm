# CycleNorm v1.1.0

App para Mac que **normaliza un 808 semiciclo por semiciclo**: detecta cada semiciclo de la onda (el positivo y el negativo, de cruce por cero a cruce por cero) y lleva cada uno al mismo pico. Así, aunque el 808 sea asimétrico (el lado negativo más chico que el positivo, o al revés), los dos lados quedan iguales. El decay desaparece y el 808 queda plano de principio a fin, pero con el timbre real de cada momento (el glide, la saturación y los armónicos de la cola se mantienen). Después lo exportás o lo arrastrás directo al DAW.

## Cómo usarla

1. Arrastrá uno o varios WAV / AIFF a la ventana, o una carpeta entera (también abre FLAC, MP3, M4A y CAF), o usá **Abrir…**. También podés soltar archivos sobre el ícono del Dock.
2. Arriba ves el **original** y abajo el **procesado**. Las líneas verticales finitas marcan cada semiciclo (u onda, según el Modo) detectado. Aparecen cuando hacés zoom y entran.
3. **Espacio** = play / stop, **A** = alternar entre escuchar el original y el procesado (sin cortar), **↑ / ↓** = pasar de archivo. **Loop** repite.
4. Para llevarlo al DAW tenés dos caminos:
   - **Exportar…** guarda el archivo seleccionado (por defecto con `_norm` al final del nombre). **Exportar todos…** procesa toda la lista con los ajustes actuales y la guarda en la carpeta que elijas.
   - **Arrastrá el procesado al DAW**: arrastrás ese botón a una pista y listo. El archivo queda guardado en `~/Music/CycleNorm/`, así el DAW lo puede seguir leyendo.

## Controles

| Control | Qué hace |
|---|---|
| **Modo** | **Semiciclo** (por defecto): el positivo y el negativo de cada onda se normalizan por separado, así los dos lados llegan al Target. **Onda entera**: cada onda completa va al Target por su pico más alto, y el lado más chico queda más chico (conserva la asimetría) |
| **Amount** | Cuánto normaliza. **100 %**: todas las ondas al mismo pico (plano). **0 %**: el original. En el medio, el decay se achica (por ejemplo, con 50 % una caída de 24 dB queda en 12 dB) |
| **Target** | El pico al que se lleva cada onda, en dBFS. Con **Auto** (por defecto) el objetivo es el pico de la primera onda que se normaliza, así el 808 mantiene el nivel con el que arranca, y la barra muestra ese valor. Sacando Auto lo elegís a mano (de -30 a +6 dB; arriba de 0 solo sirve exportando en 32 bit float) |
| **Skip (ondas)** | Las primeras ondas que no se tocan, para respetar el ataque o el click del 808. Se cuenta en ondas enteras también en modo Semiciclo (2 = 4 semiciclos). Por defecto 2 |
| **Threshold** | Por defecto **Off**: se normaliza todo, aunque suba el ruido de fondo. Si lo activás, las ondas con pico por debajo de ese nivel dejan de normalizarse y se quedan con la última ganancia: el 808 se sostiene hasta ahí y después decae naturalmente |
| **Formato** | **Igual al original** (mismo formato, bits y sample rate), **WAV 24 bit** o **WAV 32 bit float** |

Los ajustes se aplican a todos los archivos de la lista y se guardan al cerrar la app.

Abajo a la izquierda se ve cuántas ondas detectó, el objetivo que está usando y el pico de salida. Si el resultado pasa de 0 dBFS y el formato no es float, aparece **¡SATURA!** en rojo: bajá el Target o exportá en 32 bit float.

## Cómo funciona

- **Detección:** usa el canal con más energía (si L y R están en contrafase, sumarlos los anularía). Para encontrar las ondas le saca el DC y los agudos con filtros de fase cero (ida y vuelta, sin desfase), así los armónicos de un 808 saturado no agregan cruces falsos. Después corre cada cruce (el que sube y el que baja) al cruce real del audio más cercano. No usa el promedio: un 808 asimétrico tiene DC propio, y sus semiciclos empiezan y terminan donde el audio vale 0. Si cerca no hay un cruce real (pasa justo antes de un golpe nuevo), ahí no corta y une ese tramo con el vecino.
- **Ganancia:** mide el pico de cada semiciclo (u onda) en todos los canales y le aplica la ganancia que lo lleva al Target. La misma ganancia va a todos los canales, así la imagen estéreo no cambia.
- **Sin clicks:** el cambio de ganancia entre una onda y la siguiente cae justo en el cruce por cero, con una rampa muy corta (1 ms como máximo, o el 6 % de la onda).
- **Puntas del archivo:** el pedacito antes del primer cruce y el que queda después del último (una onda incompleta) se normalizan con su propio pico, sin pasarse nunca del Target.
- **Límite:** la ganancia nunca sube más de +60 dB, así el silencio digital no se lleva a 0 dB.
- **Varios golpes en un archivo:** cada golpe se normaliza al mismo objetivo. Entre golpes (si hay silencio o ruido) también se sube, salvo que uses Threshold.

## Compilar e instalar

1. En GitHub Desktop: escribí un Summary, **Commit to main**, **Push origin**. Cada push dispara la compilación; si querés guardar un cambio sin compilar, poné `[skip ci]` en el Summary.
2. Esperá la compilación en **Actions** ("Compilar app"). Antes de armar el zip, corre la prueba del motor, que tiene que dar TODO OK. Después descargá **CycleNorm.zip**.
3. Descomprimí y hacé doble clic en **Instalar.command**: copia `CycleNorm.app` a Aplicaciones, le saca el bloqueo de macOS y la abre. **Desinstalar.command** la borra junto con sus ajustes.

La app es universal (Apple Silicon e Intel) y anda en macOS 11 o más nuevo.

## Carpetas

- `Source/CycleEngine.h`: el motor (detección de ondas y ganancia). No depende de JUCE.
- `Source/Main.cpp`: la app (ventana, lista, controles, exportar).
- `Source/WaveformView.h`: el visor de forma de onda.
- `Source/ABSource.h`: la reproducción original / procesado.
- `Tests/EngineTest.cpp`: prueba del motor con 808 sintéticos (limpio, saturado con DC y ruido, estéreo invertido, 96 kHz, dos golpes, asimétrico en los dos modos, Threshold, Target fijo, Amount 50 %). Comprueba que cada tramo llega al Target, que ningún punto lo pasa, que los semiciclos cortan donde el audio vale 0 y que no aparecen clicks. Las instrucciones para compilarla están arriba del archivo.

**Visor:** la rueda o el gesto vertical de dos dedos hace zoom, el gesto horizontal desplaza, el pinch también hace zoom, un clic mueve el cursor de reproducción y el doble clic vuelve a mostrar todo.

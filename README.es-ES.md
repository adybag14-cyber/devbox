

# Devbox MCP

Devbox MCP expone un servidor MCP compatible con ChatGPT para operaciones controladas de archivos, shell, Git, Docker y herramientas del host.

Admite dos modos de ejecución:

- **Modo Docker**: el flujo de trabajo original reproducible de Windows + Docker Desktop.
- **Modo host**: ejecución directa en Termux, Linux y macOS sin Docker.

`auto` selecciona Docker en Windows y el modo host en Termux/Linux/macOS.

## Configuración más rápida: binario de inicialización en Rust

El programa multiplataforma `devbox-setup` puede clonar el repositorio o configurar un clon existente. Verifica Node.js, crea los directorios de configuración y trabajo locales, instala las dependencias, vincula el comando `devbox` cuando los permisos lo permiten, inicia el servicio MCP y verifica su punto de verificación de estado.

Los binarios precompilados se generan mediante el flujo de trabajo **Build bootstrap binaries** de GitHub Actions para:

- Windows x86-64
- Linux x86-64
- macOS x86-64
- macOS Apple Silicon
- Android/Termux arm64-v8a
- Android/Termux armeabi-v7a
- Android/Termux x86-64
- Android/Termux x86

Descargue el artefacto para su sistema operativo desde la última ejecución exitosa del flujo de trabajo, extraígalo y ejecútelo.

### Configurar un clon existente

Windows PowerShell:

```powershell
.\devbox-setup.exe --repo .
```

Linux:

```bash
chmod +x ./devbox-setup
./devbox-setup --repo .
```

macOS Apple Silicon:

```bash
chmod +x ./devbox-setup
./devbox-setup --repo .
```

### Clonar y configurar desde un directorio vacío

Ejecute el binario sin `--repo`. Clona el repositorio oficial en `./devbox` y completa la configuración:

```bash
devbox-setup
```

Opciones útiles del instalador:

```text
--runtime auto|host|docker
--host 127.0.0.1
--port 8100
--workspace /path/to/workspace
--no-start
--no-link
--skip-system-packages
--skip-install
--dry-run
```

El instalador no reemplaza un `.env` existente con `.env.example`; conserva las líneas existentes y actualiza solo las claves seleccionadas explícitamente.


### Android y Termux

Instale la aplicación canónica firmada de Termux desde:

- <https://github.com/adybag14-cyber/termux-app>
- <https://github.com/adybag14-cyber/termux-app/releases>

Luego ejecute dentro de Termux:

```bash
pkg install -y curl ca-certificates
curl --fail --location --output install-devbox.sh \
  https://raw.githubusercontent.com/adybag14-cyber/devbox/main/scripts/install-termux.sh
sh install-devbox.sh
```

El instalador elige el ABI de Android correcto, verifica e instala el binario de lanzamiento correspondiente mediante SHA-256, provisiona los paquetes necesarios de Termux, configura el modo host, inicia Devbox y verifica su punto de verificación de estado.

Instrucciones completas para Android: [docs/TERMUX.md](./docs/TERMUX.md)

## Compile el instalador de Rust usted mismo

Rust 1.74 o posterior es suficiente:

```bash
cargo test --manifest-path bootstrap/Cargo.toml
cargo build --release --manifest-path bootstrap/Cargo.toml
```

El ejecutable resultante es:

- Windows: `bootstrap/target/release/devbox-setup.exe`
- Linux/macOS: `bootstrap/target/release/devbox-setup`

Documentación completa del instalador: [bootstrap/README.md](./bootstrap/README.md)

## Qué incluye

- `bootstrap/`: binario de configuración Rust multiplataforma y pruebas
- `bin/devbox.js`: comando `devbox` instalable
- `src/server.js`: servidor MCP expuesto a través de HTTP Streamable
- `src/runtime.js`: selector de ejecución para Docker versus modo host
- `src/docker-runtime.js`: entorno de ejecución respaldado por Docker
- `src/host-runtime.js`: entorno de ejecución respaldado por el host para Termux/Linux/macOS
- `src/host-tools.js`: ayudas de ejecución para shell del host y programas permitidos
- `src/launcher.js`: lanzador de servicio en segundo plano
- `runtime.Dockerfile`: imagen de entorno de ejecución Linux reproducible para modo Docker
- `scripts/Start-ChatGptDevboxMcp.ps1`: flujo de inicio Windows/Docker
- `scripts/Stop-ChatGptDevboxMcp.ps1`: flujo de apagado Windows/Docker

## Requisitos

El binario de inicialización Rust aún necesita los prerrequisitos de ejecución utilizados por Devbox mismo.

### Todos los modos

- Node.js 18 o posterior
- npm
- Git cuando el instalador necesite clonar el repositorio

### Modo host

- Android API 21+ a través de la aplicación canónica Termux
- Termux, Linux o macOS
- opcional pero útil: `gh`, `python3`, `ripgrep` y `curl`

### Modo Docker

- Docker Desktop o un motor Docker compatible
- Windows PowerShell para los scripts de automatización de Windows proporcionados
- opcional: GitHub CLI, Cloudflare Tunnel y Cloudflare Access

## Instalación manual

```bash
git clone https://github.com/adybag14-cyber/devbox.git
cd devbox
cp .env.example .env
npm install
npm link
node bin/devbox.js start
```

En Windows PowerShell, copie el archivo de entorno con:

```powershell
Copy-Item .env.example .env
```

El repositorio carga automáticamente `<repo>/.env`. Las variables de entorno del proceso existente tienen prioridad.

## Modos de ejecución

### Modo host

El modo host es el predeterminado en Termux, Linux y macOS.

```bash
DEVBOX_RUNTIME_MODE=host devbox
```

Comportamiento:

- las operaciones de archivos y shell se ejecutan directamente en el host
- `devbox_exec_readonly` funciona en el mejor de los casos y no está aislado en un contenedor
- las herramientas genéricas del host se exponen a través de `host_*`
- los nombres heredados `windows_host_*` permanecen como alias de compatibilidad

Instrucciones para Termux y Android: [docs/TERMUX.md](./docs/TERMUX.md)

Aplicación canónica Termux: <https://github.com/adybag14-cyber/termux-app>

Detalles para Linux/macOS: [docs/HOST_COMPATIBILITY.md](./docs/HOST_COMPATIBILITY.md)

### Modo Docker

El modo Docker es el predeterminado en Windows y puede seleccionarse en otros lugares:

```bash
DEVBOX_RUNTIME_MODE=docker devbox
```

Comportamiento:

- las herramientas de shell y archivos de Devbox se ejecutan en el contenedor de entorno de ejecución Docker
- los comandos de shell de solo lectura utilizan contenedores desechables de solo lectura
- las herramientas del host permanecen explícitas y separadas del entorno de ejecución del contenedor

Los usuarios de Windows también pueden usar:

```powershell
.\scripts\Start-ChatGptDevboxMcp.ps1
```

## Configuración

Valores importantes de `.env`:

- `DEVBOX_RUNTIME_MODE=auto|host|docker`
- `HOST` y `PORT`
- `HOST_WORKSPACE_PATH`
- `HOST_DEFAULT_WORKDIR`
- `HOST_SHELL`
- `HOST_PROGRAM_ALLOWLIST`
- `PUBLIC_BASE_URL` para implementaciones públicas de OAuth
- `ENABLE_GATEWAY_BRIDGE=true|false`
- `GATEWAY_BRIDGE_ORIGINS=https://chatgpt.com,https://chat.openai.com`
- `MAX_MCP_TRANSFER_CHARS`, `MAX_TEXT_OUTPUT_CHARS` y `MCP_JSON_BODY_LIMIT` aceptan límites numéricos o `unlimited`
- `DOCKER_COMMAND_TIMEOUT_MS` controla la ejecución acotada de subprocesos de Docker

No haga commit de `.env`, `run/`, `workspace/` ni credenciales en vivo.

## Comandos de servicio

```bash
devbox status
devbox restart
devbox stop
devbox run
```

El `devbox` simple se comporta como `devbox start`. `devbox run` mantiene el servidor en primer plano.

La telemetría de ejecución se anexa a `run/tool-usage.jsonl` y `run/http-usage.jsonl`. Resúmala con `npm run usage:summary`, o ejecute la sonda de confiabilidad en vivo con `npm run soak:live`.

## Supervisor de confiabilidad Guardian v2

Guardian v2 monitorea el proceso MCP, los puntos de verificación de estado locales y públicos, el entorno de ejecución seleccionado y el túnel opcional sin que el modo host dependa de Docker. Windows usa el monitor de tareas programadas existente, Linux puede usar un servicio de usuario systemd, y Termux puede usar Termux:Boot; los tres ejecutan el mismo supervisor en primer plano.

```powershell
# Windows host mode: Docker is not probed or required
.\scripts\Install-ChatGptDevboxGuardian.ps1 -Runtime host -Public -OAuth
```

```bash
# Linux systemd user service or Termux:Boot
./scripts/install-guardian.sh auto
```

El modo Docker incluye reparación de inicio/reemplazo de contenedores inactivos, además de retroceso exponencial y un interruptor de circuito persistente tras fallos repetidos de Docker Desktop. Consulte [docs/GUARDIAN.md](./docs/GUARDIAN.md) para campos de preparación, comandos de estado y configuración del administrador de servicios.

## Valores del conector de ChatGPT

Después del inicio, inspeccione el punto de final raíz:

```bash
curl http://127.0.0.1:8100/
```

Configuraciones típicas del conector local:

- **Nombre**: `Devbox MCP`
- **MCP Server URL**: `http://127.0.0.1:8100/mcp`
- **Authentication**: `none`, `demo-oauth` o `cloudflare-access`

Cuando `MCP_AUTH_MODE=none`, las solicitudes de bucle local pueden exponer el puente del navegador para los orígenes de ChatGPT configurados. Desactívelo con `ENABLE_GATEWAY_BRIDGE=false` cuando no sea necesario.

## Notas de seguridad

- `host_exec` proporciona acceso directo al shell del host y debe habilitarse solo en un entorno confiable.
- `host_run_program` está restringido por `HOST_PROGRAM_ALLOWLIST`.
- La ejecución de solo lectura en modo host es cooperativa en lugar de un entorno aislado estricto.
- Las implementaciones públicas deben usar un modo OAuth apropiado y una URL pública configurada correctamente.

## Validación

Pruebas del servicio JavaScript:

```bash
npm test
```

Pruebas de inicialización Rust:

```bash
cargo fmt --manifest-path bootstrap/Cargo.toml -- --check
cargo test --manifest-path bootstrap/Cargo.toml
```

Prueba rápida del servicio:

```bash
node bin/devbox.js start
curl --fail http://127.0.0.1:8100/healthz
node bin/devbox.js stop
```

## Script gratuito para aprobar todos los mensajes. 

¡NO BORRAR.."!
```
let lastClicked = null;

setInterval(() => {
  const btn = [...document.querySelectorAll('button')].find(b => {
    const rect = b.getBoundingClientRect();
    const bg = getComputedStyle(b).backgroundColor;

    const sizeMatch =
      rect.width >= 195 && rect.width <= 199 &&
      rect.height >= 35 && rect.height <= 37;

    const colorMatch =
      bg === 'rgb(13, 13, 13)' ||
      bg === 'rgb(0, 0, 0)';

    return sizeMatch && colorMatch ;
  });

  if (btn && btn !== lastClicked) {
    lastClicked = btn;
    console.log('Clicking:', btn.innerText.trim(), btn.getBoundingClientRect());
    btn.click();
  }
}, 1000)
```

Si prefiere no escribir `continue` y presionar Enter, aquí tiene otro script de consola para usted. 
```
(function() {
  function getMainBoxAndButton() {
    // Find the first visible contenteditable box
    const box = Array.from(document.querySelectorAll('[contenteditable="true"]'))
                     .find(el => el.offsetParent !== null); // only visible elements

    // Try to find a send button within the same container
    let sendBtn = null;
    if (box) {
      const container = box.closest('div');
      if (container) {
        sendBtn = container.querySelector('button, input[type="submit"]');
      }
    }

    return { box, sendBtn };
  }

  function typeAndSend() {
    const { box, sendBtn } = getMainBoxAndButton();
    if (!box) {
      console.warn('No visible typing box found!');
      return;
    }

    // Focus the box
    box.focus();

    // Move cursor to the end
    const sel = window.getSelection();
    const range = document.createRange();
    range.selectNodeContents(box);
    range.collapse(false);
    sel.removeAllRanges();
    sel.addRange(range);

    // Insert the exact phrase "continue "
    document.execCommand('insertText', false, 'continue ');

    // Click send if a button exists
    if (sendBtn && !sendBtn.disabled) {
      sendBtn.click();
    } else {
      // If no button, try simulating Enter key
      const enterEvent = new KeyboardEvent('keydown', {
        key: 'Enter',
        code: 'Enter',
        keyCode: 13,
        which: 13,
        bubbles: true
      });
      box.dispatchEvent(enterEvent);
    }
  }

  // Run immediately
  typeAndSend();

  // Repeat every 2 minutes
  setInterval(typeAndSend, 2 * 60 * 1000);
})();
```
y también el script de continuación automática anterior.

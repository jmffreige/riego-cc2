param(
    [int]$Port = 8080
)

$root = [System.IO.Path]::GetFullPath($PSScriptRoot)
$listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, $Port)

$contentTypes = @{
    ".html" = "text/html; charset=utf-8"
    ".css" = "text/css; charset=utf-8"
    ".js" = "text/javascript; charset=utf-8"
    ".json" = "application/json; charset=utf-8"
    ".png" = "image/png"
    ".svg" = "image/svg+xml"
    ".ico" = "image/x-icon"
    ".txt" = "text/plain; charset=utf-8"
}

function Send-Response {
    param(
        [System.Net.Sockets.NetworkStream]$Stream,
        [int]$StatusCode,
        [string]$StatusText,
        [byte[]]$Body,
        [string]$ContentType
    )

    $headers = @(
        "HTTP/1.1 $StatusCode $StatusText"
        "Content-Type: $ContentType"
        "Content-Length: $($Body.Length)"
        "Cache-Control: no-cache"
        "Connection: close"
        ""
        ""
    ) -join "`r`n"

    $headerBytes = [System.Text.Encoding]::ASCII.GetBytes($headers)
    $Stream.Write($headerBytes, 0, $headerBytes.Length)
    $Stream.Write($Body, 0, $Body.Length)
}

try {
    $listener.Start()
    Write-Host ""
    Write-Host "Control-CC2 disponible en http://localhost:$Port" -ForegroundColor Green
    Write-Host "Pulsa Ctrl+C para detener el servidor." -ForegroundColor DarkGray
    Write-Host ""

    while ($true) {
        $client = $listener.AcceptTcpClient()

        try {
            $stream = $client.GetStream()
            $reader = [System.IO.StreamReader]::new($stream, [System.Text.Encoding]::ASCII, $false, 1024, $true)
            $requestLine = $reader.ReadLine()

            while ($reader.ReadLine()) {
                # Consumir las cabeceras de la petición.
            }

            if (-not $requestLine) {
                continue
            }

            $parts = $requestLine.Split(" ")
            $requestPath = [System.Uri]::UnescapeDataString($parts[1].Split("?")[0])

            if ($requestPath -eq "/") {
                $requestPath = "/index.html"
            }

            $relativePath = $requestPath.TrimStart("/").Replace("/", [System.IO.Path]::DirectorySeparatorChar)
            $filePath = [System.IO.Path]::GetFullPath((Join-Path $root $relativePath))

            if (-not $filePath.StartsWith($root, [System.StringComparison]::OrdinalIgnoreCase)) {
                $body = [System.Text.Encoding]::UTF8.GetBytes("403 - Acceso denegado")
                Send-Response $stream 403 "Forbidden" $body "text/plain; charset=utf-8"
                continue
            }

            if (-not [System.IO.File]::Exists($filePath)) {
                $body = [System.Text.Encoding]::UTF8.GetBytes("404 - Archivo no encontrado")
                Send-Response $stream 404 "Not Found" $body "text/plain; charset=utf-8"
                continue
            }

            $extension = [System.IO.Path]::GetExtension($filePath).ToLowerInvariant()
            $contentType = $contentTypes[$extension]

            if (-not $contentType) {
                $contentType = "application/octet-stream"
            }

            $body = [System.IO.File]::ReadAllBytes($filePath)
            Send-Response $stream 200 "OK" $body $contentType
        }
        catch {
            Write-Warning $_.Exception.Message
        }
        finally {
            if ($stream) {
                $stream.Dispose()
            }
            $client.Dispose()
        }
    }
}
finally {
    $listener.Stop()
}

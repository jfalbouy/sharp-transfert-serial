# Sharp PC-E500S -> PC
# Commande Sharp :
# OPEN "COM:9600,N,8,1,A,L,&1A,X,N"
# SAVE "COM:"
#
# Réglages validés :
# 9600,N,8,1
# XON/XOFF
# DTR = False
# RTS = True
# EOF = &1A
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$OutputFile,

    [string]$PortName = "COM1",

    [int]$IdleTimeoutMs = 3000
)

$OutputDirectory = Split-Path -Parent $OutputFile

if ($OutputDirectory -and -not (Test-Path $OutputDirectory)) {
    Write-Host "Dossier de sortie introuvable : $OutputDirectory" -ForegroundColor Red
    exit 1
}

$port = [System.IO.Ports.SerialPort]::new(
    $PortName,
    9600,
    [System.IO.Ports.Parity]::None,
    8,
    [System.IO.Ports.StopBits]::One
)

# Réglages identiques à la configuration validée côté PC -> Sharp
$port.Handshake = [System.IO.Ports.Handshake]::XOnXOff
$port.DtrEnable = $false
$port.RtsEnable = $true
$port.ReadTimeout = 100
$port.WriteTimeout = 5000

$bytes = [System.Collections.Generic.List[byte]]::new()
$started = $false
$lastReceiveTime = Get-Date

function Write-ControlByte {
    param(
        [System.IO.Ports.SerialPort]$Port,
        [byte]$Byte
    )

    $Port.Write([byte[]]($Byte), 0, 1)

    while ($Port.BytesToWrite -gt 0) {
        Start-Sleep -Milliseconds 1
    }
}

try {
    Write-Host "Ouverture de $PortName..."
    $port.Open()

    $port.DiscardInBuffer()
    $port.DiscardOutBuffer()

    Write-Host ""
    Write-Host "Fichier de sortie : $OutputFile"
    Write-Host "Configuration     : 9600,N,8,1 / XON-XOFF / RTS=ON / DTR=OFF"
    Write-Host ""
    Write-Host "Sur le Sharp PC-E500S :"
    Write-Host 'OPEN "COM:9600,N,8,1,A,L,&1A,X,N"'
    Write-Host 'SAVE "COM:"'
    Write-Host ""
    Write-Host "Important : lance d'abord ce script, puis lance SAVE côté Sharp."
    Write-Host ""

    # XON initial : autorisation d'émettre
    Write-ControlByte $port 0x11
    Write-Host "XON initial envoyé : 0x11"
    Write-Host "En attente des données Sharp..."
    Write-Host ""

    while ($true) {
        try {
            $b = $port.ReadByte()
            $started = $true
            $lastReceiveTime = Get-Date

            if ($b -eq 0x1A) {
                Write-Host ""
                Write-Host "EOF &1A reçu."
                break
            }

            $bytes.Add([byte]$b)

            if ($b -ge 32 -and $b -le 126) {
                Write-Host -NoNewline ([char]$b)
            }
            elseif ($b -eq 13) {
                Write-Host -NoNewline "`r"
            }
            elseif ($b -eq 10) {
                Write-Host -NoNewline "`n"
            }
            else {
                Write-Host -NoNewline "."
            }
        }
        catch [System.TimeoutException] {
            if (-not $started) {
                # Rien reçu pour l'instant : on attend sans spammer XON.
                continue
            }

            $elapsed = ((Get-Date) - $lastReceiveTime).TotalMilliseconds

            if ($elapsed -ge $IdleTimeoutMs) {
                Write-Host ""
                Write-Host "Timeout silence atteint : $IdleTimeoutMs ms"
                Write-Host "Arrêt de la réception sans EOF explicite."
                break
            }
        }
    }

		if ($bytes.Count -eq 0) {
			Write-Host ""
			Write-Host "Aucun octet reçu : aucun fichier créé." -ForegroundColor Yellow
			exit 2
		}

		# Résolution robuste du chemin de sortie
		$FullOutputFile = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputFile)
		$FullOutputDir  = [System.IO.Path]::GetDirectoryName($FullOutputFile)

		if (-not [string]::IsNullOrWhiteSpace($FullOutputDir)) {
			if (-not (Test-Path $FullOutputDir)) {
				New-Item -ItemType Directory -Path $FullOutputDir -Force | Out-Null
			}
		}

		Write-Host ""
		Write-Host "Chemin complet de sortie : $FullOutputFile"
		Write-Host "Nombre d'octets à écrire : $($bytes.Count)"

		[System.IO.File]::WriteAllBytes($FullOutputFile, $bytes.ToArray())

		Start-Sleep -Milliseconds 200

		if (Test-Path $FullOutputFile) {
			Write-Host "Fichier reçu : $FullOutputFile" -ForegroundColor Green
			Write-Host "Taille       : $((Get-Item $FullOutputFile).Length) octets"
		}
		else {
			Write-Host "ERREUR : le fichier n'existe pas après écriture." -ForegroundColor Red
		}
	
	
	
}
catch {
    Write-Host "Erreur : $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}
finally {
    if ($port.IsOpen) {
        $port.Close()
    }
}
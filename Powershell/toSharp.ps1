<#
.SYNOPSIS
    Transfert d'un programme BASIC texte depuis un PC Windows vers un Sharp PC-E500S.

.DESCRIPTION
    Ce script envoie un fichier BASIC ASCII vers le Sharp PC-E500S via le port serie.
    Il est destine a etre utilise avec la commande Sharp :

        OPEN "COM:9600,N,8,1,A,L,&1A,X,N"
        LOAD

    Reglages valides :
        - 9600 bauds (le debit n'accelere PAS l'emission, voir plus bas)
        - 8 bits de donnees / parite None / 1 bit d'arret
        - XON/XOFF active
        - DTR = False
        - RTS = True  <-- CRUCIAL (voir note)
        - EOF = &1A / Ctrl+Z
        - Fin de ligne = CR + LF

    Profil RAPIDE valide (~305 octets/s, x5 vs l'ancien profil) :
        - RtsEnable  = True
        - CharDelayMs = 0
        - LineDelayMs = 0
        - ChunkSize   = 16

    Note CRUCIALE sur RTS :
        Le Sharp ne transmet (donnees ET le XOFF qui nous freine) que si son entree
        CS (broche 5) est haute, pilotee par le RTS du PC (Technical Reference p.54,
        SIO send port condition = 04H). Avec RTS = False, le Sharp ne peut jamais
        renvoyer XOFF : son buffer deborde -> I/O ERROR a l'ecran. RTS = True autorise
        le flow control XON/XOFF, qui regule alors le PC a la vitesse exacte de LOAD.

    Pourquoi le debit n'aide pas :
        L'emission est limitee par la vitesse de tokenisation de LOAD (~305 octets/s,
        CPU-bound), pas par le fil. 9600 et 19200 donnent le meme temps ; on garde
        9600 pour plus de marge de timing au XOFF.

    Adaptateur USB-serie :
        Sur Prolific PL2303, mettre le FIFO "Receive Buffer" sur Low (1) dans le
        Gestionnaire de peripheriques (Parametres du port -> Avances) pour que le
        XOFF entrant soit detecte rapidement. Sur FTDI, mettre le Latency Timer a 1 ms.

    Repli si I/O ERROR (flow control indisponible) :
        Envoi cadence aveugle, lent mais increvable :
        -ChunkSize 1 -CharDelayMs 16

    Important : il n'y a AUCUN accuse de reception en PC -> Sharp. Une fin rapide ne
    prouve pas que le LOAD a reussi : verifier toujours avec LIST sur le Sharp.

.EXAMPLE
    .\toSharp.ps1 -InputFile ".\DUMP20.BAS"

.EXAMPLE
    # Repli cadence aveugle en cas d'I/O ERROR
    .\toSharp.ps1 -InputFile ".\DUMP20.BAS" -ChunkSize 1 -CharDelayMs 16
#>

param(
    # Fichier BASIC texte a envoyer vers le Sharp.
    # Le fichier doit etre en ASCII simple, sans BOM UTF-8, sans caracteres accentues.
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$InputFile,

    # Nom du port serie Windows. Par defaut : COM1.
    [string]$PortName = "COM1",

    # Debit. Le Sharp doit etre ouvert au meme debit : OPEN "COM:<baud>,...".
    # N'accelere pas l'emission (LOAD est CPU-bound). Par defaut : 9600.
    [int]$BaudRate = 9600,

    # Delai entre paquets. 0 = streaming pilote par XON/XOFF (recommande).
    # Repli aveugle : 16 (avec -ChunkSize 1).
    [int]$CharDelayMs = 0,

    # Delai entre lignes BASIC. 0 avec le flow control actif.
    [int]$LineDelayMs = 0,

    # Nombre d'octets par ecriture serie. Sans effet reel quand XON/XOFF regule.
    # Repli aveugle : 1.
    [int]$ChunkSize = 16,

    # Force RTS. True (defaut) = requis pour le flow control XON/XOFF.
    [bool]$RtsEnable = $true,

    # Si active, garde le port ouvert apres EOF jusqu'a validation manuelle (diagnostic).
    [switch]$KeepOpenAfterEof
)

# ---------------------------------------------------------------------------
# Validation du fichier d'entree
# ---------------------------------------------------------------------------

if (-not (Test-Path $InputFile)) {
    Write-Host "Fichier introuvable : $InputFile" -ForegroundColor Red
    exit 1
}

$FullInputFile = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($InputFile)

# ---------------------------------------------------------------------------
# Creation et configuration du port serie
# ---------------------------------------------------------------------------

$port = [System.IO.Ports.SerialPort]::new(
    $PortName,
    $BaudRate,
    [System.IO.Ports.Parity]::None,
    8,
    [System.IO.Ports.StopBits]::One
)

# Cote Sharp : OPEN "COM:<baud>,N,8,1,A,L,&1A,X,N" puis LOAD
# Cote PC    : <baud>,N,8,1 / XON-XOFF / DTR=False / RTS=True
$port.Handshake = [System.IO.Ports.Handshake]::XOnXOff
$port.DtrEnable = $false
$port.RtsEnable = $RtsEnable   # True = le Sharp peut renvoyer XOFF (CS haut)

$port.ReadTimeout = 50
$port.WriteTimeout = 5000

# ---------------------------------------------------------------------------
# Fonction d'envoi
# ---------------------------------------------------------------------------
#
# DelayMs = 0 : mode streaming. On ecrit et on laisse le XON/XOFF (respecte par
#               SerialPort.Write quand Handshake = XOnXOff) mettre le PC en pause
#               si le Sharp envoie XOFF. Pas d'attente ni de delai artificiel.
# DelayMs > 0 : mode cadence aveugle (repli). On attend le vidage du buffer .NET
#               puis on applique le delai. Utile si le flow control ne marche pas.
# ---------------------------------------------------------------------------

function Write-ControlledBytes {
    param(
        [Parameter(Mandatory = $true)] [System.IO.Ports.SerialPort]$Port,
        [Parameter(Mandatory = $true)] [byte[]]$Bytes,
        [Parameter(Mandatory = $true)] [int]$ChunkSize,
        [Parameter(Mandatory = $true)] [int]$DelayMs
    )

    if ($ChunkSize -lt 1) { $ChunkSize = 1 }

    for ($i = 0; $i -lt $Bytes.Length; $i += $ChunkSize) {
        $count = [Math]::Min($ChunkSize, $Bytes.Length - $i)
        $Port.Write($Bytes, $i, $count)

        if ($DelayMs -gt 0) {
            while ($Port.BytesToWrite -gt 0) {
                Start-Sleep -Milliseconds 1
            }
            Start-Sleep -Milliseconds $DelayMs
        }
    }
}

# ---------------------------------------------------------------------------
# Transfert principal
# ---------------------------------------------------------------------------

try {
    Write-Host "Ouverture de $PortName..."
    $port.Open()

    $port.DiscardInBuffer()
    $port.DiscardOutBuffer()

    $rtsLabel = if ($RtsEnable) { "ON" } else { "OFF" }
    Write-Host ""
    Write-Host "Fichier a envoyer : $FullInputFile"
    Write-Host "Configuration     : $BaudRate,N,8,1 / XON-XOFF / RTS=$rtsLabel / DTR=OFF"
    Write-Host "CharDelayMs       : $CharDelayMs"
    Write-Host "LineDelayMs       : $LineDelayMs"
    Write-Host "ChunkSize         : $ChunkSize"
    Write-Host ""
    Write-Host "Sur le Sharp PC-E500S, executer :"
    Write-Host "OPEN `"COM:$BaudRate,N,8,1,A,L,&1A,X,N`""
    Write-Host "LOAD"
    Write-Host ""

    Read-Host "Appuie sur Entree quand le Sharp est en attente sur LOAD"

    # Lecture du fichier en ASCII (le Sharp attend un programme BASIC texte).
    $lines = Get-Content $FullInputFile -Encoding ASCII

    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $totalBytes = 0

    foreach ($line in $lines) {
        # Delimiteur L cote Sharp = CR + LF.
        $payload = [System.Text.Encoding]::ASCII.GetBytes($line + "`r`n")

        Write-ControlledBytes -Port $port -Bytes $payload -ChunkSize $ChunkSize -DelayMs $CharDelayMs
        $totalBytes += $payload.Length

        if ($LineDelayMs -gt 0) {
            Start-Sleep -Milliseconds $LineDelayMs
        }
    }

    # S'assure que tout est parti avant l'EOF (necessaire en mode streaming).
    while ($port.BytesToWrite -gt 0) {
        Start-Sleep -Milliseconds 1
    }

    # EOF attendu par le Sharp : &1A = 0x1A = Ctrl+Z.
    Write-ControlledBytes -Port $port -Bytes ([byte[]](0x1A)) -ChunkSize 1 -DelayMs $CharDelayMs
    while ($port.BytesToWrite -gt 0) {
        Start-Sleep -Milliseconds 1
    }
    $totalBytes += 1

    $sw.Stop()
    $secs = [Math]::Round($sw.Elapsed.TotalSeconds, 2)
    $rate = if ($secs -gt 0) { [Math]::Round($totalBytes / $secs, 0) } else { 0 }

    Write-Host ""
    Write-Host "Emission terminee (cote PC) : $totalBytes octets en $secs s ($rate octets/s)."
    Write-Host "--> Verifier l'integrite avec LIST sur le Sharp : le PC ne recoit aucun" -ForegroundColor Yellow
    Write-Host "    accuse, une fin rapide ne prouve pas que le LOAD a reussi." -ForegroundColor Yellow

    if ($KeepOpenAfterEof) {
        Read-Host "COM reste ouvert. Appuie sur Entree quand le Sharp est revenu au prompt"
    }
    else {
        Start-Sleep -Seconds 2
    }
}
catch {
    Write-Host ""
    Write-Host "Erreur : $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}
finally {
    if ($port -and $port.IsOpen) {
        $port.Close()
    }
}

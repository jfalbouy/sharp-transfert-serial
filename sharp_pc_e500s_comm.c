#include <windows.h>
#include <mmsystem.h>   // timeBeginPeriod / timeEndPeriod (lier avec -lwinmm)
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

#ifdef _MSC_VER
#pragma comment(lib, "winmm.lib")
#endif

/*
 * Communication serie PC <-> Sharp PC-E500S.
 *
 * Cote Sharp, l'utilisateur tape manuellement :
 *   Emission PC -> Sharp :  OPEN "COM:9600,N,8,1,A,L,&H1A,X,N"  puis  LOAD
 *   Reception Sharp -> PC : OPEN "COM:9600,N,8,1,A,L,&H1A,X,N"  puis  SAVE "COM:"
 *
 * Config validee (identique aux scripts PowerShell / CE-130T) :
 *   9600,N,8,1 - XON/XOFF - EOF = &H1A - delimiteur L = CR+LF
 *   DTR = OFF (les deux sens)
 *   RTS = OFF en emission, RTS = ON en reception
 *
 * Points importants (cf. Technical Reference PC-E500 p.53-59) :
 *   - XON = 0x11, XOFF = 0x13, EOF = 0x1A
 *   - Le delimiteur "L" impose des fins de ligne CR+LF (0x0D 0x0A).
 *   - Le LOAD BASIC du Sharp n'absorbe pas les rafales : on regule le debit
 *     (envoi par petits paquets + delai), profil fiable = 1 octet / 1 ms.
 *   - Il faut terminer l'emission par l'octet EOF 0x1A.
 *   - En reception, on envoie un XON initial pour autoriser le Sharp a emettre.
 */

typedef struct {
    const char* port_name;
    DWORD baud_rate;
    int   char_delay_ms;   // delai entre paquets a l'emission (defaut 0 : flow control XON/XOFF)
    int   line_delay_ms;   // delai entre lignes BASIC (defaut 0)
    int   chunk_size;      // octets par ecriture serie (defaut 16)
    int   idle_timeout_ms; // silence max avant arret en reception (defaut 3000)
    int   rts;             // -1 = defaut selon mode ; 0 = OFF ; 1 = ON
    int   dtr;             // -1 = defaut (OFF) ; 0 = OFF ; 1 = ON
} SerialPortConfig;

// Prototypes
void print_usage(const char* program_name);
void title();
HANDLE open_sharp_port(const SerialPortConfig* config);
bool write_bytes_controlled(HANDLE hSerial, const unsigned char* data, size_t len,
                            int chunk, int char_delay_ms);
bool send_file_to_sharp(HANDLE hSerial, const char* file_path, const SerialPortConfig* config, bool verbose);
bool receive_file_from_sharp(HANDLE hSerial, const char* output_file_path, const SerialPortConfig* config, bool verbose);
bool show_serial_port_settings(const char* port_name);
bool parse_args(int argc, char* argv[], SerialPortConfig* config, char** input_file,
                char** output_file, bool* verbose, bool* show_serial, char** mode);

void title()
{
    printf("<<< sharp_comm - Transfert serie entre PC et Sharp PC-E500S     (c) 2026 Jean-Francois Albouy >>>\n\n");
    printf("Outil en ligne de commande (Windows) pour transferer des programmes BASIC en texte ASCII \nentre un PC et un ordinateur de poche Sharp PC-E500S, via une liaison serie RS-232.\n\n");
}

void print_usage(const char* program_name) {
    title();
    printf("Usage: %s [OPTIONS] --mode send <fichier> | --mode receive <fichier>\n", program_name);
    printf("Options:\n");
    printf("  -p, --port <port>       Port COM (ex: COM1, COM2). Defaut: COM1\n");
    printf("  -b, --baud <rate>       Baud rate (300..19200). Defaut: 9600\n");
    printf("  -m, --mode <mode>       Mode: 'send' ou 'receive'\n");
    printf("      --char-delay <ms>   Delai entre paquets (0 = flow control XON/XOFF). Defaut: 0\n");
    printf("      --line-delay <ms>   Delai entre lignes BASIC. Defaut: 0\n");
    printf("      --chunk <n>         Octets par ecriture serie. Defaut: 16\n");
    printf("      --idle-timeout <ms> Silence max avant arret (reception). Defaut: 3000\n");
    printf("      --rts <on|off>      Force RTS (CS du Sharp, requis pour XON/XOFF). Defaut: on\n");
    printf("      --dtr <on|off>      Force DTR (CD du Sharp). Defaut: off\n");
    printf("  -v, --verbose           Afficher les donnees a l'ecran\n");
    printf("  -s, --show-serial       Afficher les parametres du port serie\n");
    printf("  -h, --help              Affiche cette aide\n");
    printf("\nCote Sharp, taper : OPEN \"COM:9600,N,8,1,A,L,&H1A,X,N\" puis LOAD (send) ou SAVE \"COM:\" (receive)\n");
    printf("Defaut send = rapide (RTS on + XON/XOFF, ~305 o/s). Si I/O ERROR cote Sharp,\n");
    printf("repli sur l'envoi cadence aveugle : --chunk 1 --char-delay 16\n");
}

// Attend que l'utilisateur appuie sur Entree.
static void wait_enter(const char* msg) {
    printf("%s", msg);
    fflush(stdout);
    int c;
    while ((c = getchar()) != '\n' && c != EOF) { }
}

// Ouvre et configure le port serie.
HANDLE open_sharp_port(const SerialPortConfig* config) {
    HANDLE hSerial = CreateFileA(
        config->port_name,
        GENERIC_READ | GENERIC_WRITE,
        0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL
    );

    if (hSerial == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "Erreur: Impossible d'ouvrir le port %s (Code: %lu)\n",
                config->port_name, GetLastError());
        return NULL;
    }

    // Buffers internes du driver.
    SetupComm(hSerial, 4096, 4096);

    DCB dcb = {0};
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(hSerial, &dcb)) {
        fprintf(stderr, "Erreur: GetCommState a echoue (Code: %lu)\n", GetLastError());
        CloseHandle(hSerial);
        return NULL;
    }

    dcb.BaudRate = config->baud_rate;
    dcb.ByteSize = 8;
    dcb.Parity   = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary  = TRUE;
    dcb.fParity  = FALSE;

    // Handshake logiciel XON/XOFF uniquement (pas de flux materiel).
    dcb.fOutX = TRUE;   // le PC suspend l'emission si le Sharp envoie XOFF
    dcb.fInX  = TRUE;   // le PC envoie XON/XOFF selon son buffer de reception
    dcb.fOutxCtsFlow   = FALSE;
    dcb.fOutxDsrFlow   = FALSE;
    dcb.fDsrSensitivity = FALSE;
    dcb.fTXContinueOnXoff = FALSE;
    dcb.XonChar  = 0x11;
    dcb.XoffChar = 0x13;
    dcb.XonLim   = 512;
    dcb.XoffLim  = 512;

    // Lignes de controle : niveaux fixes (pas de TOGGLE).
    // Le Sharp ne transmet (donnees ET XOFF) que si son entree CS est haute,
    // pilotee par le RTS du PC (Tech Ref p.54, SIO send port condition = 04H).
    // Defaut : DTR=OFF ; RTS=ON en reception, RTS=OFF en emission (profil PS).
    // Surchargeable via --rts / --dtr pour activer le flow control en emission.
    int rts = (config->rts >= 0) ? config->rts : 1;  // defaut ON : CS du Sharp haut = XOFF possible
    int dtr = (config->dtr >= 0) ? config->dtr : 0;
    dcb.fRtsControl = rts ? RTS_CONTROL_ENABLE : RTS_CONTROL_DISABLE;
    dcb.fDtrControl = dtr ? DTR_CONTROL_ENABLE : DTR_CONTROL_DISABLE;

    if (!SetCommState(hSerial, &dcb)) {
        fprintf(stderr, "Erreur: SetCommState a echoue (Code: %lu)\n", GetLastError());
        CloseHandle(hSerial);
        return NULL;
    }

    // Timeouts : lecture qui rend la main regulierement (detection d'inactivite).
    COMMTIMEOUTS to = {0};
    to.ReadIntervalTimeout        = 50;
    to.ReadTotalTimeoutConstant   = 500;
    to.ReadTotalTimeoutMultiplier = 0;
    to.WriteTotalTimeoutConstant  = 5000;
    to.WriteTotalTimeoutMultiplier = 10;
    if (!SetCommTimeouts(hSerial, &to)) {
        fprintf(stderr, "Erreur: SetCommTimeouts a echoue (Code: %lu)\n", GetLastError());
        CloseHandle(hSerial);
        return NULL;
    }

    PurgeComm(hSerial, PURGE_TXCLEAR | PURGE_RXCLEAR);
    return hSerial;
}

// Envoi regule : petits paquets, attente de l'ecoulement physique, puis delai.
bool write_bytes_controlled(HANDLE hSerial, const unsigned char* data, size_t len,
                            int chunk, int char_delay_ms) {
    if (chunk < 1) chunk = 1;
    size_t i = 0;
    while (i < len) {
        DWORD n = (DWORD)((len - i) < (size_t)chunk ? (len - i) : (size_t)chunk);
        DWORD written = 0;
        if (!WriteFile(hSerial, data + i, n, &written, NULL)) {
            fprintf(stderr, "\nErreur: Echec de l'envoi (Code: %lu)\n", GetLastError());
            return false;
        }
        i += written;
        if (char_delay_ms > 0) {
            // Mode cadence : on draine (equiv. BytesToWrite == 0) puis on attend.
            FlushFileBuffers(hSerial);
            Sleep(char_delay_ms);
        }
        // Mode streaming (char_delay_ms == 0) : pas de drain par chunk. On laisse
        // WriteFile empiler dans le buffer driver et le XON/XOFF materiel reguler.
    }
    return true;
}

// Emission d'un fichier BASIC texte vers le Sharp.
bool send_file_to_sharp(HANDLE hSerial, const char* file_path, const SerialPortConfig* config, bool verbose) {
    FILE* file = fopen(file_path, "rb");
    if (!file) {
        fprintf(stderr, "Erreur: Impossible d'ouvrir le fichier %s\n", file_path);
        return false;
    }

    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (file_size < 0) {
        fprintf(stderr, "Erreur: Taille de fichier invalide\n");
        fclose(file);
        return false;
    }

    unsigned char* data = (unsigned char*)malloc(file_size > 0 ? (size_t)file_size : 1);
    if (!data) {
        fprintf(stderr, "Erreur: Allocation memoire echouee\n");
        fclose(file);
        return false;
    }
    size_t total = fread(data, 1, (size_t)file_size, file);
    fclose(file);

    // Valeurs effectives (defaut : RTS=ON, DTR=OFF), memes regles que open_sharp_port.
    int eff_rts = (config->rts >= 0) ? config->rts : 1;
    int eff_dtr = (config->dtr >= 0) ? config->dtr : 0;
    printf("Fichier a envoyer : %s (%ld octets)\n", file_path, file_size);
    printf("Config : %lu,N,8,1 / XON-XOFF / RTS=%s / DTR=%s\n",
           config->baud_rate,
           eff_rts ? "ON" : "OFF",
           eff_dtr ? "ON" : "OFF");
    printf("Sur le Sharp PC-E500S, taper :\n");
    printf("  OPEN \"COM:%lu,N,8,1,A,L,&H1A,X,N\"\n", config->baud_rate);
    printf("  LOAD\n");
    wait_enter("Appuyez sur Entree quand le Sharp est en attente sur LOAD...");

    PurgeComm(hSerial, PURGE_TXCLEAR | PURGE_RXCLEAR);

    const unsigned char crlf[2] = { 0x0D, 0x0A };
    size_t line_no = 0;
    size_t i = 0;
    bool ok = true;
    DWORD t_start = GetTickCount();

    // Parcourt le contenu ligne par ligne et emet chaque fin de ligne en CR+LF,
    // quel que soit le format d'origine (\r\n, \n ou \r).
    while (i < total) {
        size_t start = i;
        while (i < total && data[i] != '\n' && data[i] != '\r') {
            i++;
        }

        if (verbose) {
            printf("TX %zu: %.*s\n", ++line_no, (int)(i - start), (const char*)(data + start));
        }

        if (i > start) {
            if (!write_bytes_controlled(hSerial, data + start, i - start,
                                        config->chunk_size, config->char_delay_ms)) {
                ok = false; break;
            }
        }
        // Fin de ligne -> CR+LF (delimiteur L cote Sharp).
        if (!write_bytes_controlled(hSerial, crlf, 2, config->chunk_size, config->char_delay_ms)) {
            ok = false; break;
        }

        // Saute la sequence de fin de ligne d'origine.
        if (i < total) {
            if (data[i] == '\r' && i + 1 < total && data[i + 1] == '\n') {
                i += 2;
            } else {
                i += 1;
            }
        }

        if (config->line_delay_ms > 0) {
            Sleep(config->line_delay_ms);
        }
    }

    // EOF attendu par le Sharp : &H1A (Ctrl+Z).
    if (ok) {
        // En mode streaming (char_delay 0) rien n'a ete draine par chunk :
        // on s'assure que tout est parti avant d'envoyer l'EOF.
        FlushFileBuffers(hSerial);
        const unsigned char eof = 0x1A;
        if (verbose) printf("TX: EOF 0x1A\n");
        ok = write_bytes_controlled(hSerial, &eof, 1, 1, config->char_delay_ms);
        FlushFileBuffers(hSerial);
    }

    // Duree/debit mesures sur le transfert lui-meme (avant le delai de finalisation).
    if (ok) {
        DWORD elapsed_ms = GetTickCount() - t_start;
        double secs = elapsed_ms / 1000.0;
        double rate = secs > 0 ? total / secs : 0.0;
        printf("Emission terminee (cote PC) : %zu octets en %.2f s (%.0f octets/s).\n",
               total, secs, rate);
        printf("--> Verifier l'integrite avec LIST sur le Sharp : le PC ne recoit aucun\n");
        printf("    accuse, une fin rapide ne prouve pas que le LOAD a reussi.\n");
        // Laisse le Sharp finaliser le LOAD avant la fermeture du port.
        Sleep(2000);
    }

    free(data);
    return ok;
}

// Reception d'un fichier depuis le Sharp (SAVE "COM:").
bool receive_file_from_sharp(HANDLE hSerial, const char* output_file_path, const SerialPortConfig* config, bool verbose) {
    printf("Fichier de sortie : %s\n", output_file_path);
    printf("Config : 9600,N,8,1 / XON-XOFF / RTS=ON / DTR=OFF\n");
    printf("Sur le Sharp PC-E500S, taper :\n");
    printf("  OPEN \"COM:%lu,N,8,1,A,L,&H1A,X,N\"\n", config->baud_rate);
    printf("  SAVE \"COM:\"\n");
    wait_enter("Appuyez sur Entree pour armer la reception, PUIS lancez SAVE sur le Sharp...");

    FILE* file = fopen(output_file_path, "wb");
    if (!file) {
        fprintf(stderr, "Erreur: Impossible de creer le fichier %s\n", output_file_path);
        return false;
    }

    PurgeComm(hSerial, PURGE_TXCLEAR | PURGE_RXCLEAR);

    // XON initial : autorise le Sharp a emettre.
    {
        const unsigned char xon = 0x11;
        DWORD w = 0;
        WriteFile(hSerial, &xon, 1, &w, NULL);
        FlushFileBuffers(hSerial);
        if (verbose) printf("XON initial envoye (0x11)\nEn attente des donnees...\n");
    }

    unsigned char buffer[256];
    DWORD bytes_read = 0;
    size_t total = 0;
    bool started = false;
    bool got_eof = false;
    DWORD last = GetTickCount();
    DWORD t_first = 0;   // horodatage du premier octet recu (debit mesure a partir de la)

    while (!got_eof) {
        if (!ReadFile(hSerial, buffer, sizeof(buffer), &bytes_read, NULL)) {
            DWORD err = GetLastError();
            if (err == ERROR_OPERATION_ABORTED || err == ERROR_INVALID_HANDLE) {
                break;
            }
            fprintf(stderr, "\nErreur: Echec de la lecture (Code: %lu)\n", err);
            fclose(file);
            return false;
        }

        if (bytes_read == 0) {
            // Timeout de lecture : verifie l'inactivite.
            if (started && (GetTickCount() - last) >= (DWORD)config->idle_timeout_ms) {
                printf("\nTimeout de silence (%d ms) : arret de la reception.\n", config->idle_timeout_ms);
                break;
            }
            continue;
        }

        if (!started) {
            t_first = GetTickCount();
        }
        started = true;
        last = GetTickCount();

        // Cherche l'EOF 0x1A ; on n'ecrit pas l'octet EOF dans le fichier.
        DWORD n = bytes_read;
        for (DWORD k = 0; k < bytes_read; k++) {
            if (buffer[k] == 0x1A) {
                n = k;
                got_eof = true;
                break;
            }
        }

        if (n > 0) {
            fwrite(buffer, 1, n, file);
            total += n;
        }

        if (verbose) {
            for (DWORD k = 0; k < n; k++) {
                unsigned char c = buffer[k];
                putchar((c >= 32 && c <= 126) || c == '\r' || c == '\n' ? c : '.');
            }
            fflush(stdout);
        }
    }

    fclose(file);
    if (got_eof) {
        printf("\nEOF 0x1A recu. ");
    }
    if (total == 0) {
        printf("\nFichier recu : %s (0 octet)\n", output_file_path);
        fprintf(stderr, "Attention: aucun octet recu.\n");
        return false;
    }
    {
        DWORD elapsed_ms = GetTickCount() - t_first;
        double secs = elapsed_ms / 1000.0;
        double rate = secs > 0 ? total / secs : 0.0;
        printf("\nFichier recu : %s (%zu octets en %.2f s, %.0f octets/s)\n",
               output_file_path, total, secs, rate);
    }
    return true;
}

// Affiche les parametres actuels du port.
bool show_serial_port_settings(const char* port_name) {
    HANDLE hSerial = CreateFileA(port_name, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hSerial == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "Erreur: Impossible d'ouvrir le port %s (Code: %lu)\n", port_name, GetLastError());
        return false;
    }

    DCB dcb = {0};
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(hSerial, &dcb)) {
        fprintf(stderr, "Erreur: GetCommState a echoue (Code: %lu)\n", GetLastError());
        CloseHandle(hSerial);
        return false;
    }

    COMMTIMEOUTS to = {0};
    if (!GetCommTimeouts(hSerial, &to)) {
        fprintf(stderr, "Erreur: GetCommTimeouts a echoue (Code: %lu)\n", GetLastError());
        CloseHandle(hSerial);
        return false;
    }

    printf("\n=== Parametres du port %s ===\n", port_name);
    printf("Baud rate: %lu\n", dcb.BaudRate);
    printf("Byte size: %u\n", dcb.ByteSize);
    printf("Parity: %s\n",
           dcb.Parity == NOPARITY ? "NONE" :
           dcb.Parity == ODDPARITY ? "ODD" :
           dcb.Parity == EVENPARITY ? "EVEN" : "UNKNOWN");
    printf("Stop bits: %s\n",
           dcb.StopBits == ONESTOPBIT ? "1" :
           dcb.StopBits == ONE5STOPBITS ? "1.5" :
           dcb.StopBits == TWOSTOPBITS ? "2" : "UNKNOWN");
    printf("DTR control: %s\n",
           dcb.fDtrControl == DTR_CONTROL_DISABLE ? "DISABLE" :
           dcb.fDtrControl == DTR_CONTROL_ENABLE ? "ENABLE" : "HANDSHAKE");
    printf("RTS control: %s\n",
           dcb.fRtsControl == RTS_CONTROL_DISABLE ? "DISABLE" :
           dcb.fRtsControl == RTS_CONTROL_ENABLE ? "ENABLE" :
           dcb.fRtsControl == RTS_CONTROL_HANDSHAKE ? "HANDSHAKE" : "TOGGLE");
    printf("XON/XOFF (out): %s\n", dcb.fOutX ? "ENABLED" : "DISABLED");
    printf("XON/XOFF (in):  %s\n", dcb.fInX ? "ENABLED" : "DISABLED");
    printf("XON char: 0x%02X\n", dcb.XonChar);
    printf("XOFF char: 0x%02X\n", dcb.XoffChar);
    printf("\nTimeouts:\n");
    printf("  ReadIntervalTimeout: %lu ms\n", to.ReadIntervalTimeout);
    printf("  ReadTotalTimeoutConstant: %lu ms\n", to.ReadTotalTimeoutConstant);

    CloseHandle(hSerial);
    return true;
}

// Analyse des arguments.
bool parse_args(int argc, char* argv[], SerialPortConfig* config, char** input_file,
                char** output_file, bool* verbose, bool* show_serial, char** mode) {
    if (argc < 2) {
        print_usage(argv[0]);
        return false;
    }

    config->port_name       = "COM1";
    config->baud_rate       = CBR_9600;
    config->char_delay_ms   = 0;   // flow control XON/XOFF (necessite RTS=ON)
    config->line_delay_ms   = 0;
    config->chunk_size      = 16;
    config->idle_timeout_ms = 3000;
    config->rts             = -1;  // -1 = defaut (ON)
    config->dtr             = -1;  // -1 = defaut (OFF)
    *verbose = false;
    *show_serial = false;
    *input_file = NULL;
    *output_file = NULL;
    *mode = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--port") == 0) {
            if (i + 1 >= argc) return false;
            config->port_name = argv[++i];
        }
        else if (strcmp(argv[i], "-b") == 0 || strcmp(argv[i], "--baud") == 0) {
            if (i + 1 >= argc) return false;
            int baud = atoi(argv[++i]);
            switch (baud) {
                case 300:   config->baud_rate = CBR_300;   break;
                case 600:   config->baud_rate = CBR_600;   break;
                case 1200:  config->baud_rate = CBR_1200;  break;
                case 2400:  config->baud_rate = CBR_2400;  break;
                case 4800:  config->baud_rate = CBR_4800;  break;
                case 9600:  config->baud_rate = CBR_9600;  break;
                case 19200: config->baud_rate = CBR_19200; break;
                default: fprintf(stderr, "Erreur: Baud rate non supporte (%d)\n", baud); return false;
            }
        }
        else if (strcmp(argv[i], "--char-delay") == 0) {
            if (i + 1 >= argc) return false;
            config->char_delay_ms = atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "--line-delay") == 0) {
            if (i + 1 >= argc) return false;
            config->line_delay_ms = atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "--chunk") == 0) {
            if (i + 1 >= argc) return false;
            config->chunk_size = atoi(argv[++i]);
            if (config->chunk_size < 1) config->chunk_size = 1;
        }
        else if (strcmp(argv[i], "--idle-timeout") == 0) {
            if (i + 1 >= argc) return false;
            config->idle_timeout_ms = atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "--rts") == 0) {
            if (i + 1 >= argc) return false;
            const char* v = argv[++i];
            if (strcmp(v, "on") == 0) config->rts = 1;
            else if (strcmp(v, "off") == 0) config->rts = 0;
            else { fprintf(stderr, "Erreur: --rts attend 'on' ou 'off'\n"); return false; }
        }
        else if (strcmp(argv[i], "--dtr") == 0) {
            if (i + 1 >= argc) return false;
            const char* v = argv[++i];
            if (strcmp(v, "on") == 0) config->dtr = 1;
            else if (strcmp(v, "off") == 0) config->dtr = 0;
            else { fprintf(stderr, "Erreur: --dtr attend 'on' ou 'off'\n"); return false; }
        }
        else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            *verbose = true;
        }
        else if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--show-serial") == 0) {
            *show_serial = true;
        }
        else if (strcmp(argv[i], "-m") == 0 || strcmp(argv[i], "--mode") == 0) {
            if (i + 1 >= argc) return false;
            *mode = argv[++i];
            if (strcmp(*mode, "send") != 0 && strcmp(*mode, "receive") != 0) {
                fprintf(stderr, "Erreur: Mode invalide. Utilisez 'send' ou 'receive'\n");
                return false;
            }
        }
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            exit(0);
        }
        else {
            if (*mode == NULL) {
                fprintf(stderr, "Erreur: Le mode ('send' ou 'receive') doit etre specifie avec -m/--mode\n");
                return false;
            }
            if (strcmp(*mode, "send") == 0) {
                if (*input_file == NULL) *input_file = argv[i];
                else { fprintf(stderr, "Erreur: Un seul fichier d'entree en mode 'send'\n"); return false; }
            } else {
                if (*output_file == NULL) *output_file = argv[i];
                else { fprintf(stderr, "Erreur: Un seul fichier de sortie en mode 'receive'\n"); return false; }
            }
        }
    }

    if (*show_serial) return true;
    if (*mode == NULL) { fprintf(stderr, "Erreur: Le mode ('send' ou 'receive') doit etre specifie\n"); return false; }
    if (strcmp(*mode, "send") == 0 && *input_file == NULL) { fprintf(stderr, "Erreur: Fichier d'entree requis en mode 'send'\n"); return false; }
    if (strcmp(*mode, "receive") == 0 && *output_file == NULL) { fprintf(stderr, "Erreur: Fichier de sortie requis en mode 'receive'\n"); return false; }

    return true;
}

int main(int argc, char* argv[]) {
    SerialPortConfig config;
    char* input_file = NULL;
    char* output_file = NULL;
    bool verbose = false;
    bool show_serial = false;
    char* mode = NULL;

    if (!parse_args(argc, argv, &config, &input_file, &output_file, &verbose, &show_serial, &mode)) {
        return 1;
    }

    if (show_serial) {
        return show_serial_port_settings(config.port_name) ? 0 : 1;
    }

    bool is_receive = (strcmp(mode, "receive") == 0);
    HANDLE hSerial = open_sharp_port(&config);
    if (!hSerial) {
        return 1;
    }

    // Resolution timer 1 ms : sans cela, Sleep(1) dure ~15,6 ms (granularite
    // par defaut de Windows), ce qui plafonne l'emission independamment du baud.
    // Avec cette resolution, --char-delay/--line-delay refletent des vraies ms.
    timeBeginPeriod(1);

    bool ok;
    if (is_receive) {
        ok = receive_file_from_sharp(hSerial, output_file, &config, verbose);
    } else {
        ok = send_file_to_sharp(hSerial, input_file, &config, verbose);
    }

    timeEndPeriod(1);
    CloseHandle(hSerial);
    if (ok && verbose) {
        printf("[FIN] Communication avec le SHARP PC-E500S terminee.\n");
    }
    return ok ? 0 : 1;
}

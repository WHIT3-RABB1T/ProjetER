# Architecture du firmware — documentation

Ce document explique en français l'organisation du code et de l'arborescence
de fichiers du projet STM32CubeIDE **ProjetER**, le firmware qui tourne sur
la carte **B-U585I-IOT02A** (STM32U585, Discovery kit IoT). Pour la
documentation du serveur HTTPS qui reçoit ses données, voir
`Doc/SERVER.md`.

## 1. Vue d'ensemble

La carte lit en continu sa suite de capteurs embarqués, envoie le résultat
en HTTPS à un serveur (`tools/http_server.py`, sur un PC du même réseau
Wi-Fi), et sert aussi de manette de jeu (bouton + accéléromètre/gyroscope)
pour les jeux du tableau de bord web de ce serveur. Le tout tourne sur
**ThreadX** (RTOS temps réel) avec **NetX Duo** (pile réseau) et
**NetX Secure** (TLS) — toute la partie « Azure RTOS » vient de la suite
Eclipse ThreadX, distribuée par ST dans `STM32Cube_FW_U5`.

## 2. Arborescence des dossiers

| Dossier | Contenu | Fourni par |
|---|---|---|
| `Core/Src`, `Core/Inc` | Code spécifique à ce projet : capteurs, provisionnement Wi-Fi, point d'entrée `main()`, configuration HAL/ThreadX générée par CubeMX. | Projet + généré par CubeMX |
| `NetXDuo/App` | Le cœur applicatif réseau : `app_netxduo.c` (threads, HTTPS, watchdog), la configuration TLS (`nx_secure_user.h`, `nx_user.h`), le certificat compilé en dur (`https_ca_cert.h`). | Projet (au-dessus d'un squelette CubeMX) |
| `NetXDuo/Target` | Configuration de la couche d'abstraction RTOS pour le pilote Wi-Fi (`mx_wifi_azure_rtos_conf.h`). | CubeMX / vendeur |
| `AZURE_RTOS/App` | Amorçage générique ThreadX/NetX Duo généré par CubeMX (création des pools mémoire, appel de `App_ThreadX_Init` puis `MX_NetXDuo_Init`). | Généré par CubeMX, quasiment jamais modifié |
| `Application/User` | Callbacks newlib (`syscalls.c`/`sysmem.c`) et le fichier de démarrage assembleur. | Généré par CubeMX |
| `Drivers/BSP` | Pilotes des capteurs et du support de carte (`B-U585I-IOT02A`, `Components/{hts221,lps22hh,ism330dhcx,iis2mdc,veml3235,vl53l5cx,mx_wifi}`). | Vendeur (ST), copié tel quel de `STM32Cube_FW_U5` |
| `Drivers/STM32U5xx_HAL_Driver`, `Drivers/CMSIS` | HAL et CMSIS standard ST/ARM. | Vendeur |
| `Middlewares/ThreadX`, `Middlewares/NetXDuo`, `Middlewares/ST/{threadx,netxduo}` | Le RTOS et la pile réseau eux-mêmes (ThreadX, NetX Duo, NetX Secure, l'add-on DHCP). | Vendeur — **mais patché par endroits, voir §8** |
| `tools/` | Le serveur HTTPS côté PC et le tableau de bord web — voir `Doc/SERVER.md`. | Projet |
| `Doc/` | Ce document. | Projet |

Règle générale : tout ce qui est sous `Drivers/` et `Middlewares/` est du
code fournisseur copié depuis le paquet `STM32Cube_FW_U5`, à l'exception de
quelques correctifs ciblés (§8). Le code propre au projet vit dans
`Core/`, `NetXDuo/App/`, et `tools/`.

## 3. Séquence de démarrage

1. `Core/Src/main.c` — `main()` : `HAL_Init()`, horloges, puis
   initialisation des périphériques générés par CubeMX : GPIO, ICACHE,
   **SPI2** (bus du module Wi-Fi EMW3080), **USART1** (UART de debug —
   c'est la même liaison série que `printf()` utilise, et maintenant aussi
   celle du provisionnement Wi-Fi, voir §6), et le **RNG** matériel (utilisé
   par NetX Secure pour la génération de nombres aléatoires TLS). Puis
   `MX_ThreadX_Init()`, qui appelle `tx_kernel_enter()` — le contrôle ne
   revient jamais dans `main()`.
2. `AZURE_RTOS/App/app_azure_rtos.c` — `tx_application_define()` (appelé
   automatiquement par ThreadX au démarrage du noyau) : crée les pools
   mémoire (byte pool ThreadX, pool de paquets NetX), puis appelle
   `App_ThreadX_Init()` (`Core/Src/app_threadx.c`, généré — actuellement
   vide de code utilisateur) et **`MX_NetXDuo_Init()`**.
3. `NetXDuo/App/app_netxduo.c` — `MX_NetXDuo_Init()` : c'est ici que
   commence vraiment la partie spécifique au projet (voir §4 et §5).

## 4. Les threads ThreadX

Créés dans `MX_NetXDuo_Init()` :

- **`AppMainThread`** (`App_Main_Thread_Entry`, priorité `APP_THREAD_PRIORITY`,
  démarrage automatique) : initialise les capteurs (`Sensors_Init()`),
  lance le DHCP, attend une adresse IP, puis crée et démarre
  `AppHTTPThread`.
- **`AppHTTPThread`** (`App_HTTP_Thread_Entry`, même priorité, démarré
  manuellement par `AppMainThread`) : la boucle principale — découverte du
  serveur, connexion TLS/HTTPS persistante, lecture des capteurs, envoi
  d'un `POST /sensors`, lecture de la réponse, recommence. Voir §5 et §7.
- **Le pilote Wi-Fi (`mx_wifi_spi_txrx_task`)**, créé par le middleware
  vendeur (`Drivers/BSP/Components/mx_wifi`), tourne à la priorité **la
  plus haute de toute l'application** (`OSPRIORITYREALTIME`,
  `mx_wifi_conf.h`) — voir §8 pour pourquoi c'est important.
- **Un minuteur de diagnostic** (`DiagHeartbeatTimer`, `diag_heartbeat_entry`)
  tourne dans le contexte du minuteur système de ThreadX (donc indépendant
  de la priorité des threads applicatifs) pendant les appels bloquants
  d'`AppHTTPThread`, pour distinguer un vrai blocage RTOS d'un thread de
  priorité supérieure qui monopolise le CPU — voir §9.
- **Un chien de garde matériel (IWDG)**, réarmé par `watchdog_timer_entry`
  tant qu'`AppHTTPThread` progresse normalement, redémarre la carte si ce
  thread reste bloqué trop longtemps (`WATCHDOG_STALE_TICKS`).

## 5. Lecture des capteurs (`Core/Src/sensors.c`)

`Sensors_Init()` initialise les six capteurs du bus I2C2 (HTS221,
LPS22HH, ISM330DHCX, IIS2MDC, VEML3235, et VL53L5CX — ce dernier
actuellement désactivé, voir le commentaire dans `sensors.c` : son
initialisation BSP rend le bus I2C2 inutilisable pour les autres capteurs).
Chaque capteur est sondé indépendamment ; l'échec de l'un n'empêche pas les
autres de fonctionner.

`Sensors_ReadAllJSON()` construit **un seul objet JSON combiné** par tour
de lecture (une clé par catégorie ayant une valeur ce tour-ci :
`temperature`, `humidity`, `pressure`, `accelerometer`, `gyroscope`,
`magnetometer`, `light`, `button`) — délibérément un seul `POST` HTTPS
plutôt qu'un par capteur, parce qu'une poignée de main TLS complète coûte
~500-650 ms sur cette carte (RSA-2048, aucune accélération matérielle) ;
faire une poignée de main par capteur aurait rendu un tour complet 3,5 à
5 fois plus lent.

Le bouton USER physique (broche PC13, GPIO simple, pas un capteur I2C) est
inclus dans le même objet JSON sous la clé `button`, pour être disponible
depuis le même flux `/api/latest` que tout le reste — c'est ce qui permet
aux jeux du tableau de bord (Apple Catch, Course, Tilt) d'utiliser le
bouton physique de la carte.

## 6. Réseau : Wi-Fi, provisionnement, découverte

- **Provisionnement Wi-Fi** (`Core/Src/wifi_provisioning.c`,
  `WifiProvisioning_Init()`, appelé au tout début de `MX_NetXDuo_Init()`,
  avant `nx_ip_create()`) : au lieu de coder en dur le SSID/mot de passe
  dans le firmware (l'ancienne approche, avec le mot de passe versionné
  dans le dépôt git), les identifiants sont soit lus depuis une page flash
  dédiée (dernière page de 8 Ko, 0x081FE000), soit saisis au clavier via un
  terminal série sur USART1 (la même liaison que le debug) si aucun réseau
  n'est enregistré ou si le bouton USER est maintenu au démarrage. Une
  tentative précédente avec un portail captif (point d'accès + serveur
  DHCP/HTTP embarqués sur la carte) a été abandonnée après un vrai bug dans
  l'add-on DHCP serveur du fournisseur (mémoire tampon fixe de 12 octets,
  débordée par la liste d'options d'un client réel).
- **Découverte automatique du serveur** (`Discover_ServerIP()`,
  `app_netxduo.c`, voir aussi `Doc/SERVER.md` §4) : la carte diffuse une
  requête UDP en broadcast au démarrage (et si elle perd la connexion), le
  serveur y répond, et la carte lit l'adresse IP du serveur directement
  depuis la réponse — plus besoin de la coder en dur
  (`HTTP_SERVER_ADDRESS` dans `app_netxduo.h` ne sert plus que de secours
  après ~30 s sans réponse).

## 7. HTTPS côté carte (`App_HTTP_Thread_Entry`)

Contrairement à une implémentation naïve qui referait une connexion
TLS/HTTP à chaque envoi, `App_HTTP_Thread_Entry` garde une seule connexion
TCP+TLS ouverte et réutilisée pour chaque tour (voir aussi
`Doc/SERVER.md` §9 côté serveur) — c'est ce qui permet à la carte
d'envoyer ses lectures aussi vite que possible malgré le coût d'une
poignée de main RSA-2048 complète.

Le certificat du serveur (`tools/certs/server.crt`, généré par
`tools/gen_https_cert.sh`) est copié en dur dans
`NetXDuo/App/https_ca_cert.h` : NetX Secure vérifie que le certificat reçu
correspond **exactement, octet par octet**, à cette copie — ce n'est pas
une vérification de chaîne de confiance classique (pas de vérification de
nom d'hôte/SAN), donc regénérer le certificat après un changement d'IP est
purement cosmétique tant que le fichier `.h` et `tools/certs/server.crt`
restent synchronisés.

Les suites de chiffrement disponibles côté carte sont limitées par NetX
Secure (`nx_secure_user.h` / `nx_crypto_tls_ciphers`) à TLS 1.2 avec
échange de clé RSA statique (pas d'ECC ni de chiffrement authentifié) —
voir `Doc/SERVER.md` §2 pour pourquoi le serveur doit s'y adapter
explicitement.

## 8. Correctifs appliqués aux middlewares fournisseur

Plusieurs bugs de synchronisation ont été trouvés et corrigés directement
dans le code vendeur vendored (`Middlewares/ST/netxduo`,
`Drivers/BSP/Components/mx_wifi`), après des blocages reproductibles sur du
vrai matériel :

- **Mutex non bornés** : plusieurs fonctions vendeurs
  (`nx_secure_tls_session_start.c`, `nx_secure_tls_session_receive_records.c`,
  `nx_tcp_socket_send_internal.c`, `nx_tcp_socket_receive.c`) appelaient
  `tx_mutex_get(&mutex, TX_WAIT_FOREVER)` sans jamais respecter le délai
  d'attente (`wait_option`) que l'appelant leur avait pourtant passé —
  corrigé pour utiliser ce délai et retourner une erreur proprement plutôt
  que de bloquer indéfiniment.
- **Boucle d'allocation de tampon non bornée dans le pilote Wi-Fi**
  (`mx_wifi_spi.c`, `process_txrx_poll()`) : si le pool de paquets partagé
  venait à manquer au mauvais moment, cette boucle tournait indéfiniment —
  or ce thread tourne à la priorité **la plus haute** de toute
  l'application (voir §4), donc un blocage ici privait tous les autres
  threads du CPU, y compris `AppHTTPThread`, sans que son propre minuteur
  de garde ne serve à rien. Bornée à un nombre fixe de tentatives.
- **Poll SPI sans délai** (`mx_wifi_spi_txrx_task`, même fichier) :
  appelait `process_txrx_poll(WAIT_FOREVER)`, ce qui rendait aussi
  l'appel bas niveau `HAL_SPI_TransmitReceive()` sans limite de temps —
  en mode polling (pas de DMA configuré dans ce projet), cet appel HAL
  boucle en occupant le CPU sans jamais rendre la main à l'ordonnanceur ;
  un vrai blocage du bus SPI ou du module Wi-Fi y bloquait donc tout,
  de façon indétectable autrement que par le chien de garde matériel.
  Bornée à un délai fixe (`MX_WIFI_SPI_POLL_TIMEOUT_MS`).

Ces trois classes de bug produisaient le même symptôme observable : le
minuteur de diagnostic continuait de s'exécuter à l'heure (preuve que le
tick RTOS fonctionnait normalement), mais `AppHTTPThread` restait figé
plusieurs secondes avant que le chien de garde matériel (IWDG, indépendant
de ThreadX) ne redémarre la carte.

## 9. Instrumentation de diagnostic

`app_netxduo.c` contient plusieurs variables de diagnostic, imprimées à
chaque battement du minuteur (`diag_heartbeat_entry`) :

- **`RoundStep`** — à quelle étape du tour `AppHTTPThread` en est
  (`post_secure_start`, `request_packet_allocate`, `put_packet`,
  `response_body_get`, ...).
- **`DiagTlsSubStep`** — pour distinguer, *à l'intérieur même* de
  `nx_secure_tls_session_receive_records.c` (middleware vendeur), quel
  appel précis est bloqué.

Ces deux variables ont permis d'isoler les bugs décrits au §8 sans accès
direct à un débogueur JTAG en continu — juste une capture série.

## 10. Voir aussi

- `Doc/SERVER.md` — le serveur HTTPS côté PC, l'API du tableau de bord,
  et le protocole TLS vu de ce côté-là.
- `NetXDuo/App/app_netxduo.c` — les commentaires en tête de fichier
  détaillent l'historique complet de chaque bug de synchronisation trouvé
  sur ce projet, avec les preuves qui ont mené à chaque diagnostic.

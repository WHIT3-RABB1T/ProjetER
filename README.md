# ProjetER — capteurs, HTTPS et tableau de bord sur STM32U585

Firmware pour la carte **B-U585I-IOT02A** (STM32U585, Discovery kit IoT) qui lit
en continu ses capteurs embarqués et les envoie, en HTTPS, à un serveur tournant
sur un ordinateur du même réseau Wi-Fi. Ce serveur affiche les mesures dans un
tableau de bord web, où la carte sert aussi de manette pour plusieurs petits jeux.

Le projet est construit sur **Azure RTOS** (ThreadX, NetX Duo, NetX Secure) avec
STM32CubeIDE.

## Ce que fait le projet

- **Capteurs** : température, humidité, pression, accéléromètre, gyroscope,
  magnétomètre, lumière, et le bouton USER de la carte, envoyés ensemble dans un
  seul objet JSON par `POST /sensors`.
- **HTTPS (TLS 1.2)** : une seule connexion chiffrée, réutilisée pour tous les
  envois. La carte vérifie que le certificat du serveur est identique, octet par
  octet, à celui compilé dans son firmware (`NetXDuo/App/https_ca_cert.h`).
- **Découverte automatique du serveur** : au démarrage, la carte diffuse une
  requête UDP (port 7000) et lit l'adresse IP du serveur dans la réponse — plus
  aucune adresse à coder en dur.
- **Configuration Wi-Fi sans recompiler** : le SSID et le mot de passe sont saisis
  une fois via le port série (USART1) et enregistrés en mémoire flash.
- **Tableau de bord web** servi par `tools/http_server.py`, avec six onglets :
  Dashboard (une carte par capteur, avec historique en graphique), Cryptographie
  (chiffré/déchiffré réel du dernier échange de la carte), Démo (orientation 3D
  en direct), Tilt (labyrinthe), Catch (attraper des pommes) et Race (course).
- **Robustesse** : chien de garde matériel (IWDG), minuteur de diagnostic, et
  correctifs appliqués à des bugs de blocage trouvés dans les middlewares du
  fournisseur (voir `Doc/STM32.md`).

## Documentation

Toute la documentation est en français, dans le dossier [`Doc/`](Doc/) :

| Fichier | Contenu |
|---|---|
| [`Doc/INSTALLATION.md`](Doc/INSTALLATION.md) | Guide pas à pas pour installer et faire fonctionner le projet sur un ordinateur neuf (la carte étant déjà programmée). |
| [`Doc/ARCHITECTURE.md`](Doc/ARCHITECTURE.md) | Schémas de l'architecture, des phases de vie de la carte et des échanges carte ↔ serveur. |
| [`Doc/STM32.md`](Doc/STM32.md) | Organisation du code et des dossiers du firmware, threads, capteurs, correctifs des middlewares. |
| [`Doc/SERVER.md`](Doc/SERVER.md) | Fonctionnement détaillé du serveur `tools/http_server.py`. |

## Démarrage rapide (carte déjà programmée)

1. Sur l'ordinateur, lancez le serveur depuis le dossier `tools/` :
   - **Linux** : `docker compose up --build`
   - **Windows / Mac** : `python http_server.py` (Windows) ou `python3 http_server.py` (Mac)

   Sous Windows et Mac, Docker ne convient pas : la découverte automatique de la
   carte ne peut pas y fonctionner (voir `Doc/SERVER.md`, §11).
2. Donnez le réseau Wi-Fi à la carte via le port série au premier démarrage
   (maintenez le bouton USER au démarrage pour changer de réseau plus tard).
3. Ouvrez l'adresse `https://<IP de l'ordinateur>:8443/` affichée par le serveur
   (le navigateur signale un certificat auto-signé : c'est normal).

Ne régénérez **pas** `tools/certs/` (`gen_https_cert.sh`) si la carte est déjà
programmée : le certificat doit correspondre exactement à celui compilé dans son
firmware. Le détail est dans [`Doc/INSTALLATION.md`](Doc/INSTALLATION.md).

## Compiler et programmer la carte

Nécessaire seulement pour modifier le firmware :

1. Ouvrez le projet dans **STM32CubeIDE** (fichier `Nx_UDP_Echo_Client.ioc` /
   dossier du projet).
2. Compilez (`Project > Build All`) et programmez la carte via l'ST-LINK intégré
   (le même câble USB sert au débogage et au port série).
3. Si vous changez de certificat (`tools/gen_https_cert.sh`), le script regénère
   aussi `NetXDuo/App/https_ca_cert.h` : il faut recompiler et reprogrammer la
   carte, sinon elle refusera le nouveau serveur.

## Structure du dépôt

| Dossier | Contenu |
|---|---|
| `Core/` | Code propre au projet : `main.c`, capteurs (`sensors.c`), provisionnement Wi-Fi (`wifi_provisioning.c`), configuration HAL/ThreadX. |
| `NetXDuo/App/` | Cœur applicatif réseau : `app_netxduo.c` (threads, découverte, client HTTPS, chien de garde), configuration TLS/NetX. |
| `AZURE_RTOS/`, `Application/` | Amorçage ThreadX/NetX généré par CubeMX et fichiers de démarrage. |
| `Drivers/`, `Middlewares/` | Code fournisseur (HAL, BSP capteurs, pilote Wi-Fi, ThreadX, NetX Duo), avec quelques correctifs ciblés. |
| `tools/` | Serveur HTTPS, tableau de bord (`dashboard.html`), certificats, Dockerfile et fichiers `docker-compose`. `udp_listener.py` est un ancien utilitaire de test UDP, sans rapport avec le firmware actuel. |
| `Doc/` | Documentation en français. |

## Environnement matériel et logiciel

- Carte : **B-U585I-IOT02A** (MB1551-U585AI), testée en Révision D01.
- Module Wi-Fi **MXCHIP EMW3080B**, qui ne sait pas se connecter à un réseau
  **5 GHz** : utilisez un réseau 2,4 GHz. Le pilote du module est dans
  `Drivers/BSP/Components/mx_wifi` (version indiquée dans son
  `Release_Notes.html`) et nécessite le firmware de module **V2.3.4** ; une carte
  livrée avec le firmware V2.1.11 doit être mise à jour via
  [X-WIFI-EMW3080B](https://www.st.com/en/development-tools/x-wifi-emw3080b.html)
  (fichier `EMW3080update_B-U585I-IOT02A-RevC_V2.3.4_SPI.bin`, dossier V2.3.4/SPI).
- Port série (USART1, journaux et saisie du Wi-Fi) : 115200 bauds, 8 bits,
  1 bit d'arrêt, sans parité, sans contrôle de flux.
- Serveur : Python 3 (bibliothèque standard uniquement ; le module optionnel
  `cryptography` n'est utile que pour l'ancien point d'accès `/api/cert-info`) ou
  Docker sous Linux.

## Conseils ThreadX propres à ce projet

- ThreadX utilise le Systick comme base de temps : le HAL utilise donc une base de
  temps distincte via un timer TIM (`stm32u5xx_hal_timebase_tim.c`).
- Ici, ThreadX tourne à **1000 ticks/s** (`TX_TIMER_TICKS_PER_SECOND` dans
  `Core/Inc/tx_user.h`, et non les 100 par défaut) : un tick vaut 1 ms.
- ThreadX désactive les interruptions pendant son démarrage : les appels système
  (HAL, BSP) se font dans les fonctions d'entrée des threads, pas dans
  `tx_application_define()`, qui ne doit créer que des ressources ThreadX.
- Le pilote Wi-Fi tourne à la priorité la plus haute de l'application : une boucle
  non bornée dans ce pilote prive tous les autres threads du CPU (voir
  `Doc/STM32.md`, §8).

## Mots-clés

RTOS, Network, ThreadX, NetXDuo, NetX Secure, TLS, HTTPS, WIFI, MXCHIP, UART, STM32U5

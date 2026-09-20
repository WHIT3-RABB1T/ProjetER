# `http_server.py` — documentation

Ce document explique en français le fonctionnement de `tools/http_server.py`,
le serveur HTTPS qui reçoit les données envoyées par la carte STM32
(`NetXDuo/App/app_netxduo.c`, `App_HTTP_Thread_Entry`) et qui sert le tableau
de bord web (`dashboard.html`).

## 1. Rôle général

La carte effectue, en boucle, un `POST` HTTPS vers `/sensors` contenant un
objet JSON unique regroupant toutes les catégories de capteurs ayant une
lecture ce tour-ci (`Core/Src/sensors.c`, `Sensors_ReadAllJSON()`) :
`temperature`, `humidity`, `pressure`, `accelerometer`, `gyroscope`,
`magnetometer`, `light`, `button` (et `ranging`, désactivé côté carte — voir
plus bas).

`http_server.py` a trois responsabilités :

1. **Recevoir et journaliser** ces lectures (`do_POST`), et les garder en
   mémoire pour le tableau de bord (`_latest_reading`, `_history`).
2. **Servir `dashboard.html`** et une petite API JSON dont ce fichier a
   besoin (`/api/latest`, `/api/history`, `/api/crypto-sample`,
   `/api/cert-info`).
3. **Répondre aux requêtes de découverte UDP** de la carte, pour qu'elle
   trouve l'adresse IP du serveur toute seule au démarrage.

Tout le fichier est en pure bibliothèque standard, à l'exception du module
`cryptography` (optionnel, seulement pour la fiche du certificat).

## 2. Pourquoi TLS 1.2 avec des suites de chiffrement précises

Le pile TLS de la carte (NetX Secure, `NetXDuo/App/nx_secure_user.h`) ne sait
faire que du TLS 1.2, sans ECC ni chiffrement authentifié (AEAD) : seulement
`TLS_RSA_WITH_AES_128_CBC_SHA256` et `TLS_RSA_WITH_AES_256_CBC_SHA256` (noms
OpenSSL : `AES128-SHA256` / `AES256-SHA256`). Un client TLS moderne (OpenSSL
récent, un navigateur) négocierait par défaut du TLS 1.3 ou une suite ECDHE,
que la carte ne comprend pas du tout — `main()` fixe donc explicitement la
version (`ctx.minimum_version = ctx.maximum_version = TLSv1_2`) et la liste
des suites.

Le serveur ajoute quand même deux suites ECDHE modernes
(`ECDHE-RSA-AES128-GCM-SHA256`, `ECDHE-RSA-AES256-GCM-SHA384`) à la liste :
un navigateur récent refuse catégoriquement de négocier une suite sans
confidentialité persistante (« forward secrecy ») comme celles de la carte —
sans ces deux suites, ouvrir le tableau de bord depuis un vrai navigateur
échouerait purement au niveau TLS (`SSL_ERROR_NO_CYPHER_OVERLAP`), avant même
d'arriver à l'avertissement habituel de certificat auto-signé. La carte,
elle, n'offre toujours que ses deux suites RSA dans son propre
`ClientHello` ; le même certificat RSA 2048 bits signe les deux types
d'échange de clé, donc un seul certificat suffit pour les deux mondes.

## 3. Le certificat et sa confiance

`tools/gen_https_cert.sh` génère la paire certificat/clé
(`tools/certs/server.crt` / `.key`), auto-signée, valable 10 ans (la carte
n'a pas d'horloge temps réel fiable). La carte ne fait *pas* une vérification
classique de chaîne de confiance : elle compare octet par octet le
certificat reçu avec sa propre copie compilée en dur
(`NetXDuo/App/https_ca_cert.h`, généré par ce même script). Regénérer le
certificat après un changement d'adresse IP est donc purement cosmétique
côté carte (le sujet du certificat contient l'IP, mais rien ne la vérifie
réellement) — ce qui compte, c'est que le fichier `.h` et
`tools/certs/server.crt` restent identiques.

## 4. Découverte automatique par UDP (`discovery_responder`)

Plutôt que de coder en dur l'adresse IP de ce serveur dans le firmware (ce
qui cassait à chaque redémarrage du point d'accès mobile), la carte diffuse
en broadcast UDP une requête de découverte au démarrage (et si elle perd la
connexion en cours de route) : `Discover_ServerIP()` dans `app_netxduo.c`.

`discovery_responder()` tourne dans un thread daemon séparé (démarré depuis
`main()`), écoute sur le port UDP `DISCOVERY_PORT` (7000, doit rester
identique à `app_netxduo.h`), et répond avec `DISCOVERY_REPLY` dès qu'elle
reçoit exactement `DISCOVERY_REQUEST`. La carte lit l'adresse IP du serveur
directement depuis l'adresse source du paquet de réponse — rien dans le
contenu du message lui-même — donc l'adresse actuelle de cette machine n'a
jamais besoin d'être saisie où que ce soit.

Une adresse de secours (`HTTP_SERVER_ADDRESS`/`HTTP_SERVER_HOST` dans
`app_netxduo.h`) reste utilisée si la découverte échoue après plusieurs
tentatives.

## 5. Réception des données (`Handler.do_POST`)

Pour chaque `POST /sensors` :

1. Lecture de exactement `Content-Length` octets sur le socket
   (`self.rfile.read(length)`) — une erreur ou un timeout est journalisée
   proprement puis la connexion est abandonnée, sans faire planter le
   serveur.
2. Tentative de décodage JSON. Un corps non-JSON (ancien firmware envoyant
   un simple compteur décimal, par exemple) est simplement affiché tel
   quel plutôt que de faire échouer la requête.
3. Pour chaque catégorie du dictionnaire reçu, le formateur correspondant
   dans `CATEGORY_FORMATTERS` construit la ligne de journal (repli sur
   `format_generic` — un simple `clé=valeur` — pour une catégorie inconnue).
4. La lecture est stockée dans l'état partagé (`_latest_reading`,
   `_latest_at_ms`, `_latest_source`, `_history`) sous `_state_lock`, pour
   que `dashboard.html` puisse la lire via `/api/latest` et `/api/history`.
5. Réponse `200 ack\n` envoyée au client.
6. Après l'envoi de la réponse (pas avant), `_publish_crypto_sample` capture
   le chiffré/déchiffré réel de cet échange pour l'onglet Cryptographie du
   tableau de bord.

`_history` est une file (`collections.deque`) bornée à la fois en durée
(`HISTORY_WINDOW_SECONDS`, 10 minutes) et en nombre d'entrées
(`HISTORY_MAX_ENTRIES`) — un outil de diagnostic à court terme, pas un
enregistrement de données à long terme.

## 6. Les routes GET (`Handler.do_GET`)

| Chemin | Rôle |
|---|---|
| `/` | Sert `dashboard.html`, relu depuis le disque à chaque requête (pas de cache en mémoire), pour que modifier le fichier prenne effet immédiatement au rechargement du navigateur. `Cache-Control: no-store` empêche aussi le navigateur lui-même de garder une vieille copie. |
| `/api/latest` | La dernière lecture reçue, son horodatage, l'IP source, et les paramètres TLS négociés par cette même IP (`_tls_by_ip`) — pollé toutes les 500 ms par `dashboard.html` pour les cartes du tableau de bord principal. |
| `/api/history` | L'historique récent, filtré par `?category=<nom>` si fourni — alimente le graphique ouvert en cliquant sur une carte. |
| `/api/cert-info` | Analyse `tools/certs/server.crt`/`.key` (à la volée, via le module `cryptography`) pour l'onglet Cryptographie — actuellement inutilisé par `dashboard.html` (la section Certificat/Clé a été retirée de l'interface), mais toujours disponible pour inspection directe. |
| `/api/crypto-sample` | Le chiffré/déchiffré réel de la dernière connexion de la carte (voir section 8), affiché en direct dans l'onglet Cryptographie. |
| tout autre chemin | Un simple message texte de type "Hello from `<hostname>`, you asked for `<path>`" — utile pour vérifier rapidement l'accessibilité du serveur avec `curl`. |

## 7. `HTTPSServer` : un thread par connexion, avec timeout

`HTTPSServer` hérite de `socketserver.ThreadingMixIn` et `HTTPServer`, et
enveloppe **chaque connexion acceptée** individuellement en TLS (dans
`get_request()`), plutôt que le socket d'écoute lui-même. Deux raisons,
documentées en détail dans le code :

- **Isolation des échecs de handshake** : envelopper le socket d'écoute fait
  que `accept()` échoue (ou pire, se bloque) dès qu'un client envoie un
  `ClientHello` malformé, avant même que la boucle de `socketserver` ait pu
  isoler le problème. En enveloppant par connexion, un mauvais handshake
  n'affecte que cette connexion précise.
- **Un bug réel observé sur du vrai matériel** : sans `ThreadingMixIn` ni
  timeout, le serveur (mono-thread par défaut) pouvait se retrouver bloqué
  indéfiniment sur un `recv()` qui ne se termine jamais (une connexion TCP
  établie mais dont le handshake TLS ne progresse plus) — plus aucune
  connexion suivante n'était alors jamais traitée, et il fallait tuer puis
  relancer le processus pour s'en sortir. `ThreadingMixIn` (chaque connexion
  sur son propre thread) plus `CONNECTION_TIMEOUT_SECONDS` (posé avant même
  le handshake TLS, donc valable aussi pour chaque lecture/écriture dans
  `do_GET`/`do_POST`) referment ce problème des deux côtés à la fois.

`handle_error()` est surchargée pour ne pas afficher une trace complète à
chaque déconnexion normale (la carte qui redémarre, change de réseau, ou
relance une découverte UDP ailleurs) — seule une erreur réellement inattendue
(pas une `OSError`/`ConnectionError`) affiche encore la trace complète.

## 8. `_TLSConnection` : voir le chiffré, pour de vrai

`ssl.SSLContext.wrap_socket()` (utilisé partout ailleurs habituellement) fait
tout son travail — handshake, lecture, écriture — directement dans le code C
d'OpenSSL, sans jamais faire transiter les octets chiffrés par du code
Python observable. Impossible, donc, de les afficher dans l'onglet
Cryptographie avec cette approche.

`_TLSConnection` (avec `ssl.MemoryBIO`) fait le travail manuellement :
pomper les octets chiffrés entre le socket brut et la machine à états TLS
soi-même (`_pump_in`/`_pump_out`), en gardant une copie de chaque paquet dans
`self.chunks` (avec sa direction — `c2s` carte→serveur ou `s2c`
serveur→carte — et un horodatage). Elle implémente juste assez de méthodes
(`recv`, `sendall`, `makefile`, `settimeout`, `cipher`, `version`,
`shutdown`, `close`) pour remplacer un `ssl.SSLSocket` de façon transparente
— tout le reste du serveur (`do_GET`/`do_POST`, le threading, le timeout)
n'a rien besoin de savoir de ce changement.

`_parse_tls_records` découpe ensuite ces octets bruts en enregistrements TLS
individuels (en-tête de 5 octets : type, version, longueur) pour construire
le diagramme de séquence du handshake — juste assez de logique pour nommer
chaque message (`ClientHello`, `Certificate`, ...), pas un analyseur TLS
complet.

`_publish_crypto_sample`, appelée uniquement après un vrai `POST /sensors`
réussi (jamais depuis une requête `GET` du navigateur), sépare les octets du
handshake (capturés une seule fois, à l'ouverture de la connexion) de ceux
de l'échange applicatif (remis à zéro après chaque capture, puisque la
connexion reste ouverte et réutilisée pour de nombreux tours grâce à
`Handler.protocol_version = "HTTP/1.1"`).

## 9. Pourquoi la connexion HTTP reste ouverte

`Handler.protocol_version = "HTTP/1.1"` (au lieu du `"HTTP/1.0"` par défaut
de `BaseHTTPRequestHandler`) permet à la carte de réutiliser la même
connexion TCP+TLS pour chaque `POST /sensors`, au lieu de refaire un
handshake RSA-2048 complet à chaque fois (~500-650 ms, sans accélération
matérielle sur la carte — c'était la vraie limite de fréquence d'envoi,
documentée en détail dans `App_HTTP_Thread_Entry`, `app_netxduo.c`). Le
client HTTP de NetX Duo (`nx_web_http_client.c`) détecte tout seul cette
possibilité en lisant la ligne de statut de la réponse, à condition que le
serveur déclare bien `HTTP/1.1`.

`TCP_NODELAY` est aussi activé sur chaque socket accepté
(`HTTPSServer.get_request()`) : une fois la connexion réutilisée pour de
nombreux petits aller-retours, l'algorithme de Nagle (qui retarde les petits
paquets pour les regrouper) ajoutait des dizaines de millisecondes de retard
mesurable à chaque tour.

## 10. Lancement

```bash
python3 tools/gen_https_cert.sh          # une fois, ou après un changement d'IP
python3 tools/http_server.py             # écoute sur 0.0.0.0:8443
python3 tools/http_server.py --port 8443
```

Options (`main()`, via `argparse`) :

- `--host` (défaut `0.0.0.0`)
- `--port` (défaut `8443`, doit correspondre à `HTTP_SERVER_HTTPS_PORT`
  dans `app_netxduo.h`)
- `--certfile` / `--keyfile` (défaut : `tools/certs/server.crt`/`.key`)

Arrêt avec Ctrl+C.

## 11. Avec Docker

`tools/Dockerfile` et `tools/docker-compose.yml` empaquettent ce serveur :
`docker compose up --build` dans `tools/` construit l'image et démarre le
conteneur, avec `tools/certs/` monté en volume (le certificat/la clé doivent
correspondre exactement à ce que le firmware actuellement flashé sur la carte
a en dur dans `https_ca_cert.h`, donc ils ne sont pas intégrés à l'image
elle-même).

**Le conteneur utilise `network_mode: host` (Linux uniquement).** La carte
retrouve le serveur par un *broadcast* UDP et lit l'adresse IP du serveur
dans l'adresse source de la réponse. Avec des ports publiés classiques
(`ports: 7000:7000/udp`), le relais UDP de Docker (`docker-proxy`) fait bien
arriver le broadcast dans le conteneur, mais ne sait pas renvoyer la réponse à
l'expéditeur d'un broadcast : la carte ne reçoit jamais rien, relance sa
requête toutes les 2 s, et ne se connecte jamais (constaté : le journal du
serveur affiche des « Discovery request ... replying » en boucle, sans aucune
connexion TLS). En mode `host`, le conteneur partage la pile réseau de la
machine et se comporte exactement comme `http_server.py` lancé directement.

Docker Desktop (Windows/Mac) n'offre pas ce mode : le serveur doit y être
lancé directement avec Python (`python http_server.py`, voir
`Doc/INSTALLATION.md`). `tools/docker-compose.bridge.yml` (ports publiés)
existe pour ces plateformes, mais uniquement pour le tableau de bord et les
`POST` — pas pour la découverte automatique.

## 12. Fichiers liés, côté carte

- `NetXDuo/App/app_netxduo.c` / `.h` — `App_HTTP_Thread_Entry` (le client
  HTTPS), `Discover_ServerIP()` (la découverte UDP), `HTTP_SERVER_ADDRESS`
  (l'adresse de secours).
- `NetXDuo/App/https_ca_cert.h` — la copie compilée en dur du certificat,
  générée par `gen_https_cert.sh`.
- `NetXDuo/App/nx_secure_user.h` — la table des suites de chiffrement que la
  pile NetX Secure de la carte sait négocier.
- `Core/Src/sensors.c` — `Sensors_ReadAllJSON()`, qui construit le JSON
  envoyé à chaque `POST /sensors`.

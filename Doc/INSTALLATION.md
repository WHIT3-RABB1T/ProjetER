# Guide d'installation 

Ce guide explique, étape par étape, comment faire fonctionner. Il ne suppose aucune connaissance
technique préalable.

**Ce qui est déjà fait :** la carte électronique (le "board") est déjà
programmée ("flashée") — vous n'avez pas besoin de STM32CubeIDE, ni de
recompiler ou reprogrammer quoi que ce soit sur la carte elle-même. Ce
guide s'occupe uniquement de tout ce qui se passe **côté ordinateur**, et
de la connexion Wi-Fi entre la carte et cet ordinateur.

Suivez les étapes dans l'ordre. Chaque étape indique exactement quoi taper
et à quoi vous attendre.

---

## Ce dont vous avez besoin avant de commencer

- Un ordinateur (Windows, Mac ou Linux) connecté à Internet.
- La carte électronique, déjà programmée, avec son câble USB.
- Un réseau Wi-Fi que **la carte ET l'ordinateur** pourront tous les deux
  rejoindre (un partage de connexion depuis un téléphone fonctionne très
  bien, tant qu'il est en **2,4 GHz** — le module Wi-Fi de la carte ne sait
  pas se connecter à un réseau 5 GHz. Sur la plupart des téléphones, il
  existe une option "bande 2,4 GHz" ou "compatibilité" dans les réglages du
  partage de connexion).
- Un accès à ce projet (un lien vers la page GitHub, ou le dossier du
  projet fourni autrement, par exemple sur une clé USB).

---

## Étape 1 — Récupérer le projet

1. Ouvrez la page du projet sur GitHub dans votre navigateur.
2. Cliquez sur le bouton vert **"Code"**, puis **"Download ZIP"**.
3. Une fois le fichier téléchargé, faites un clic droit dessus et choisissez
   **"Extraire tout..."** (Windows) ou double-cliquez dessus (Mac) pour
   obtenir un dossier normal, par exemple `ProjetER`.
4. Retenez bien l'endroit où vous avez extrait ce dossier (par exemple
   `Documents\ProjetER`) — vous en aurez besoin à l'étape 3.

> Si vous êtes à l'aise avec Git, l'équivalent est simplement :
> `git clone <adresse-du-dépôt>`

**Important :** ce dossier contient déjà, dans `tools/certs/`, le fichier
de sécurité (certificat) qui correspond exactement à ce qui a été
enregistré dans la carte au moment où elle a été programmée. Ne le
supprimez pas et ne le régénérez pas (n'exécutez pas
`gen_https_cert.sh`) — sinon la carte ne fera plus confiance au serveur et
refusera de s'y connecter.

---

## Étape 2 — Installer le logiciel qui fait tourner le serveur

Le serveur du projet est un petit programme Python. Selon votre ordinateur,
il se lance de deux façons différentes.

> **Pourquoi deux façons ?** La carte retrouve le serveur toute seule en
> envoyant un message "y a-t-il un serveur ici ?" à tout le réseau
> (broadcast). Docker sur Windows et Mac isole le serveur dans une machine
> virtuelle qui **ne peut pas répondre correctement à ce message** — la carte
> ne trouverait jamais le serveur. Sur Linux, Docker peut partager
> directement la connexion réseau de l'ordinateur, et tout fonctionne.

**Windows ou Mac → installez Python (méthode recommandée)**

1. Allez sur <https://www.python.org/downloads/> et téléchargez la dernière
   version de Python 3.
2. Lancez l'installateur. **Sur Windows, cochez absolument la case
   "Add python.exe to PATH"** en bas de la première fenêtre, puis cliquez
   sur "Install Now". (Sans cette case, l'étape 4 ne fonctionnera pas.)
3. Aucune autre installation n'est nécessaire : le serveur n'utilise que ce
   que Python fournit déjà.

**Linux → installez Docker**

1. Installez Docker Engine et le plugin Compose en suivant le guide officiel
   pour votre distribution : <https://docs.docker.com/engine/install/>
2. Vérifiez qu'il fonctionne en tapant `docker compose version` dans un
   terminal.

---

## Étape 3 — Ouvrir un terminal dans le dossier du projet

Un "terminal" (ou "invite de commandes") est une fenêtre où l'on tape des
commandes au lieu de cliquer sur des icônes.

**Sur Windows :**
1. Ouvrez le dossier `ProjetER` que vous avez extrait à l'étape 1, puis
   entrez dans le sous-dossier `tools`.
2. Dans la barre d'adresse de l'explorateur de fichiers (en haut de la
   fenêtre, où s'affiche le chemin du dossier), tapez `cmd` puis appuyez
   sur Entrée. Une fenêtre noire s'ouvre : c'est le terminal, déjà placé
   dans le bon dossier.

**Sur Mac :**
1. Ouvrez l'application **Terminal** (Cmd+Espace, tapez "Terminal").
2. Tapez `cd ` (avec un espace après), puis glissez-déposez le dossier
   `tools` du projet dans la fenêtre du Terminal, puis appuyez sur Entrée.

**Sur Linux :** ouvrez votre terminal habituel, puis :
```bash
cd chemin/vers/ProjetER/tools
```

---

## Étape 4 — Lancer le serveur

Dans le terminal ouvert à l'étape précédente, tapez la commande qui
correspond à votre ordinateur, puis appuyez sur Entrée :

**Windows :**
```
python http_server.py
```
(Si Windows répond que `python` est introuvable, essayez `py http_server.py`.)

**Mac :**
```
python3 http_server.py
```

**Linux (avec Docker) :**
```
docker compose up --build
```
La première fois, cela peut prendre quelques minutes (Docker télécharge et
prépare tout ce dont il a besoin).

**Sur Windows**, une fenêtre du pare-feu peut apparaître : cliquez sur
**"Autoriser l'accès"** (réseaux privés). Sans cela, la carte ne pourra pas
joindre le serveur.

Vous devriez voir apparaître des lignes ressemblant à ceci :

```
Listening on TLS 1.2 (AES128-SHA256:AES256-SHA256) 0.0.0.0:8443
Certificate: (chemin vers)/tools/certs/server.crt
This machine's LAN-facing IP looks like: 192.168.1.42
  -> the board finds this on its own via UDP discovery (port 7000); no need to hardcode it.
Dashboard: https://192.168.1.42:8443/ (browser will warn on the self-signed cert -- proceed past it)
Waiting for requests from the board... Ctrl+C to stop.
```

**Retenez l'adresse affichée après "Dashboard:"** (ici,
`https://192.168.1.42:8443/`) — vous en aurez besoin à l'étape 6. Elle sera
différente sur votre réseau.

Laissez cette fenêtre de terminal ouverte : c'est le serveur qui tourne.
Le fermer arrête le serveur.

---

## Étape 5 — Connecter la carte à votre réseau Wi-Fi

Cette étape n'est nécessaire que si la carte n'a **jamais** été connectée à
ce réseau Wi-Fi précis auparavant (par exemple, un nouveau partage de
connexion, ou un nouveau mot de passe). Si le réseau Wi-Fi n'a pas changé
depuis la dernière fois que la carte a fonctionné, la carte s'y reconnecte
automatiquement toute seule — passez directement à l'étape 6 et voyez si
ça fonctionne.

Sinon, il faut donner le nouveau nom et mot de passe du Wi-Fi à la carte,
une seule fois, via une liaison série (le même câble USB que celui déjà
branché à la carte).

### 5.1 — Installer un logiciel de terminal série

- **Windows :** installez [PuTTY](https://www.putty.org/) (gratuit).
- **Mac :** utilisez l'application Terminal déjà installée.
- **Linux :** utilisez l'application Terminal déjà installée.

### 5.2 — Trouver le port de la carte

Branchez la carte à l'ordinateur avec son câble USB si ce n'est pas déjà
fait.

- **Windows :** ouvrez le "Gestionnaire de périphériques" (recherchez-le
  dans le menu Démarrer), dépliez la section "Ports (COM et LPT)". Vous
  devriez voir une ligne du type `STMicroelectronics STLink Virtual COM
  Port (COM5)` — le numéro (`COM5` ici) est ce qu'il vous faut noter, il
  peut être différent sur votre ordinateur.
- **Mac :** ouvrez le Terminal et tapez `ls /dev/tty.usb*` — vous verrez
  apparaître un nom du type `/dev/tty.usbmodem14103`.
- **Linux :** ouvrez le Terminal et tapez `ls /dev/ttyACM*` — vous verrez
  apparaître un nom du type `/dev/ttyACM0`.

### 5.3 — Ouvrir la liaison série

**Avec PuTTY (Windows) :**
1. Lancez PuTTY.
2. Choisissez le type de connexion **"Serial"**.
3. Dans "Serial line", entrez le port trouvé à l'étape précédente (ex.
   `COM5`).
4. Dans "Speed", entrez `115200`.
5. Cliquez sur **"Open"**.

**Avec le Terminal (Mac/Linux) :**
```bash
screen /dev/tty.usbmodem14103 115200
```
(remplacez le nom du port par celui que vous avez trouvé).

Une fenêtre noire, vide ou avec du texte qui défile, s'ouvre : c'est la
liaison directe avec la carte.

### 5.4 — Redémarrer la carte et répondre aux questions

Appuyez sur son bouton "RESET" pour la redémarrer, tout en gardant la
fenêtre de la liaison série ouverte.

Du texte va défiler. À un moment, vous verrez :

```
WiFi provisioning: hold USER button now to reprovision Wi-Fi...
```

**Si vous voulez changer de réseau Wi-Fi**, appuyez tout de suite sur le
bouton bleu "USER" de la carte et maintenez-le enfoncé une seconde. Vous
verrez alors apparaître :

```
=== WiFi Setup ===
USER button held at boot -- reprovisioning.
Enter WiFi SSID:
```

Tapez le nom exact de votre réseau Wi-Fi (attention aux majuscules/minuscules),
puis appuyez sur Entrée. Il vous sera ensuite demandé :

```
Enter WiFi Password (leave blank for an open network):
```

Tapez le mot de passe du réseau, puis appuyez sur Entrée (si le réseau n'a
pas de mot de passe, appuyez directement sur Entrée sans rien taper).

La carte continue alors son démarrage et essaie de se connecter avec ces
nouvelles informations — cette information est enregistrée dans la carte
et sera réutilisée automatiquement à chaque démarrage suivant, tant que
vous ne recommencez pas cette étape.

> Si vous ne faites rien pendant les quelques secondes du message
> "hold USER button now...", la carte réutilise automatiquement le dernier
> réseau enregistré, sans rien vous demander.

Vous pouvez fermer la fenêtre de la liaison série une fois cette étape
terminée — elle ne sert qu'à cette configuration ponctuelle.

---

## Étape 6 — Ouvrir le tableau de bord

1. Ouvrez un navigateur web (Chrome, Firefox, Edge...) sur votre
   ordinateur — celui qui fait tourner le serveur (étape 4), ou n'importe
   quel autre appareil connecté au **même réseau Wi-Fi**.
2. Tapez l'adresse notée à l'étape 4 (par exemple
   `https://192.168.1.42:8443/`).
3. Le navigateur affiche un avertissement de sécurité ("Votre connexion
   n'est pas privée", ou similaire) — **c'est normal**, le certificat est
   auto-signé spécifiquement pour ce projet, il n'y a pas de vraie faille
   de sécurité. Cliquez sur **"Paramètres avancés"** puis
   **"Continuer vers... (dangereux)"** (le texte exact varie selon le
   navigateur).
4. Le tableau de bord s'affiche. Si la carte est allumée, connectée au bon
   réseau Wi-Fi, et à portée, ses données (température, capteurs, etc.)
   doivent apparaître au bout de quelques secondes.

---

## Récapitulatif

| Étape | Une seule fois, ou à chaque redémarrage ? |
|---|---|
| 1. Récupérer le projet | Une seule fois |
| 2. Installer Python (Windows/Mac) ou Docker (Linux) | Une seule fois |
| 3-4. Lancer le serveur | À chaque fois que vous voulez utiliser le projet (`python http_server.py` ou `docker compose up --build`, dans `tools/`) |
| 5. Connecter la carte au Wi-Fi | Une seule fois par réseau Wi-Fi (pas besoin de recommencer si le réseau ne change pas) |
| 6. Ouvrir le tableau de bord | À chaque fois |

---

## Problèmes courants

**Le tableau de bord ne montre aucune donnée.**
- Vérifiez que la carte est bien alimentée (une LED doit être allumée).
- Vérifiez que la carte et l'ordinateur sont bien sur le **même** réseau
  Wi-Fi.
- Vérifiez que le réseau est bien en 2,4 GHz, pas en 5 GHz.
- Attendez 30 secondes : la carte essaie plusieurs fois de retrouver le
  serveur avant d'abandonner.

**Le serveur affiche une erreur de port déjà utilisé ("Address already in use" ou
"port is already allocated").**
- Un autre programme utilise déjà le port 8443 sur cet ordinateur (peut-être
  une ancienne copie du serveur encore ouverte dans un autre terminal).
  Fermez-la, ou redémarrez l'ordinateur.

**Le pare-feu Windows demande une autorisation au premier lancement.**
- Acceptez ("Autoriser l'accès") — c'est nécessaire pour que la carte et
  le navigateur puissent joindre le serveur sur le réseau local.

**La carte n'affiche jamais le message "WiFi provisioning..." dans la
liaison série.**
- Vérifiez le port choisi (étape 5.2) et la vitesse (`115200`).
- Essayez de redémarrer la carte à nouveau une fois la liaison série déjà
  ouverte (le message n'apparaît qu'au démarrage, si la liaison est ouverte
  après, vous le manquez).

**Pour aller plus loin :** voir `Doc/STM32.md` (comment le code du
firmware est organisé) et `Doc/SERVER.md` (comment le serveur
fonctionne en détail).

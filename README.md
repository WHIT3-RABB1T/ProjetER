## <b>Description de l'application Nx_UDP_Echo_Client</b>

Cette application fournit un exemple d'utilisation de la pile Azure RTOS NetX/NetXDuo.

Elle montre comment développer un client UDP NetX qui communique avec un serveur distant à l'aide de l'API de sockets UDP de NetX.

La fonction d'entrée principale tx_application_define() est appelée par ThreadX au démarrage du noyau ; c'est à ce moment que toutes les ressources NetX sont créées.

 + Un <i>NX_PACKET_POOL</i> est alloué

 + Une instance <i>NX_IP</i> utilisant ce pool est initialisée

 + Les protocoles <i>ARP</i>, <i>ICMP</i> et <i>UDP</i> sont activés pour l'instance <i>NX_IP</i>

 + Un <i>client DHCP est créé.</i>

L'application crée ensuite 2 threads de même priorité :

 + **AppMainThread** (priorité 10, PreemtionThreashold 10) : créé avec l'indicateur <i>TX_AUTO_START</i> pour démarrer automatiquement.

 + **AppUDPThread** (priorité 10, PreemtionThreashold 10) : créé avec l'indicateur <i>TX_DONT_START</i> pour être démarré plus tard.

**AppMainThread** démarre et effectue les actions suivantes :

  + Démarre le client DHCP

  + Attend la résolution de l'adresse IP

  + Relance **AppUDPThread**

**AppUDPThread**, une fois démarré :

  + Crée un socket client <i>UDP</i>

  + Se connecte au serveur UDP distant sur le port prédéfini

  + Si la connexion réussit, le client UDP envoie MAX_PACKET_COUNT messages au serveur.

  + À chaque message envoyé, le client UDP lit la réponse du serveur et l'affiche sur l'HyperTerminal, et la LED verte change d'état.


####  <b>Comportement attendu en cas de succès</b>

 + L'adresse IP de la carte est affichée sur l'HyperTerminal

 + Les messages de réponse envoyés par le serveur sont affichés sur l'HyperTerminal

 + Si l'utilitaire [echotool](https://github.com/PavelBansky/EchoTool/releases/tag/v1.5.0.0) est utilisé, les messages envoyés par le client sont affichés dans la console du PC.
 
 + Un message récapitulatif similaire au suivant est affiché sur l'HyperTerminal et la LED verte clignote.

 ```
  SUCCESS : 100 / 100 packets sent
```

#### <b>Comportements en cas d'erreur</b>

+ La LED rouge clignote pour indiquer qu'une erreur s'est produite, tandis que la LED verte est éteinte.

+ Si l'échange de messages n'est pas terminé, l'HyperTerminal n'affiche pas les messages reçus.

#### <b>Hypothèses éventuelles</b>

Aucune

#### <b>Limitations connues</b>

Aucune

#### <b>Conseils d'utilisation de ThreadX</b>

 - ThreadX utilise le Systick comme base de temps ; il est donc obligatoire que le HAL utilise une base de temps distincte via les IP TIM.

 - ThreadX est configuré par défaut à 100 ticks/s, ce dont il faut tenir compte lors de l'utilisation de délais ou de timeouts dans l'application. Il est toujours possible de le reconfigurer dans « tx_user.h », via la définition « TX_TIMER_TICKS_PER_SECOND », mais cela doit aussi être répercuté dans le fichier « tx_initialize_low_level.S ».

 - ThreadX désactive toutes les interruptions pendant le démarrage du noyau pour éviter tout comportement inattendu ; par conséquent, tous les appels système (HAL, BSP) doivent être effectués soit au début de l'application, soit à l'intérieur des fonctions d'entrée des threads.

 - ThreadX propose la fonction « tx_application_define() », appelée automatiquement par l'API tx_kernel_enter().
   Il est vivement recommandé de l'utiliser pour créer toutes les ressources ThreadX de l'application (threads, sémaphores, pools mémoire...), mais elle ne doit en aucun cas contenir d'appel à une API système (HAL ou BSP).
 
 - L'utilisation de l'allocation mémoire dynamique nécessite d'apporter quelques modifications au fichier de l'éditeur de liens.

   ThreadX doit transmettre à la fonction tx_application_define() un pointeur vers le premier emplacement mémoire libre en RAM,
   via l'argument « first_unused_memory ».
   Cela nécessite des modifications dans les fichiers de l'éditeur de liens pour exposer cet emplacement mémoire.
   
    + Pour EWARM, ajoutez la section suivante dans le fichier .icf :
     ```
     place in RAM_region    { last section FREE_MEM };
     ```
    + Pour MDK-ARM :
    ```
    soit définir la région RW_IRAM1 dans le fichier « .sct »
    soit modifier la ligne ci-dessous dans « tx_initialize_low_level.S » pour qu'elle corresponde à la région mémoire utilisée
        LDR r1, =|Image$$RW_IRAM1$$ZI$$Limit|
    ```
    + Pour STM32CubeIDE, ajoutez la section suivante dans le fichier .ld :
    ```
    ._threadx_heap :
      {
         . = ALIGN(8);
         __RAM_segment_used_end__ = .;
         . = . + 64K;
         . = ALIGN(8);
       } >RAM_D1 AT> RAM_D1
    ```

       Le moyen le plus simple de fournir de la mémoire à ThreadX est de définir une nouvelle section, voir ._threadx_heap ci-dessus.
       Dans l'exemple ci-dessus, la taille du tas ThreadX est fixée à 64 Ko.
       La section ._threadx_heap doit être placée entre les sections .bss et ._user_heap_stack dans le script de l'éditeur de liens.
       Attention : assurez-vous que ThreadX n'a pas besoin de plus de mémoire de tas que celle fournie (64 Ko dans cet exemple).
       Pour en savoir plus, consultez le guide de l'utilisateur de STM32CubeIDE, chapitre : « Linker script ».

    + Le fichier « tx_initialize_low_level.S » doit également être modifié pour activer l'indicateur « USE_DYNAMIC_MEMORY_ALLOCATION ».

### <b>Mots-clés</b>

RTOS, Network, ThreadX, NetXDuo, WIFI, UDP, MXCHIP, UART

### <b>Environnement matériel et logiciel</b>

 - Pour utiliser les fonctionnalités du module Wi-Fi MXCHIP EMW3080B, 2 composants logiciels sont nécessaires :
   1. Le pilote du module, qui s'exécute sur le composant STM32
   2. Le firmware du module, qui s'exécute sur le module Wi-Fi EMW3080B

 - Cette application utilise une version mise à jour du pilote V2.3.4 du module Wi-Fi MXCHIP EMW3080B.

 - La carte Discovery B-U585I-IOT02A Révision D est livrée avec le firmware V2.1.11 du module Wi-Fi MXCHIP EMW3080B ;
   pour mettre à jour votre carte vers la version V2.3.4 requise, rendez-vous sur [X-WIFI-EMW3080B](https://www.st.com/en/development-tools/x-wifi-emw3080b.html),
   en utilisant le fichier `EMW3080update_B-U585I-IOT02A-RevC_V2.3.4_SPI.bin` dans le dossier V2.3.4/SPI.

 - Veuillez noter que le firmware V2.1.11 du module n'est pas rétrocompatible avec le pilote V2.3.4 (le firmware V2.1.11 du module est compatible avec les versions du pilote de V2.1.11 à V2.1.13).
 - Le pilote du module est disponible dans [/Drivers/BSP/Components/mx_wifi](../../../../../Drivers/BSP/Components/mx_wifi/), et sa version est indiquée dans le fichier [Release_Notes.html](../../../../../Drivers/BSP/Components/mx_wifi/Release_Notes.html).

 - Sachez que certains paquets logiciels STM32U5 (par exemple X-CUBE-AZURE et X-CUBE-AWS) peuvent continuer à utiliser d'anciennes versions du firmware du module MXCHIP EMW3080B ;
   veuillez vous référer aux notes de version de chaque paquet logiciel pour connaître la version de firmware du module recommandée, qui peut être obtenue sur cette page :
   [X-WIFI-EMW3080B](https://www.st.com/en/development-tools/x-wifi-emw3080b.html).

 - Cette application a été testée avec des cartes B-U585I-IOT02A (MB1551-U585AI) Révision : Rev D01 et peut facilement être adaptée à tout autre composant et carte de développement pris en charge.

 - Cette application utilise USART1 pour afficher les journaux ; la configuration de l'hyperterminal est la suivante :
      - Vitesse = 115200 bauds
      - Longueur de mot = 8 bits
      - Bit d'arrêt = 1
      - Parité = Aucune
      - Contrôle de flux = Aucun


###  <b>Comment l'utiliser ?</b>

Pour faire fonctionner le programme, vous devez procéder comme suit :

 - Ouvrez votre chaîne d'outils préférée

 - Dans <code> Core/Inc/mx_wifi_conf.h </code>, modifiez vos paramètres Wi-Fi (WIFI_SSID,WIFI_PASSWORD)

 - Modifiez le fichier <code> NetXDuo/App/app_netxduo.h</code> et mettez à jour <i>DEFAULT_PORT</i> avec le port sur lequel se connecter.

 - Lancez l'utilitaire [echotool](https://github.com/PavelBansky/EchoTool/releases/tag/v1.5.0.0) dans une console Windows comme suit :

       c:\> .\echotool.exe /p udp /s <UDP_SERVER_PORT> 
           
       exemple : c:\> .\echotool.exe /p udp /s 6000 

   (PS : le serveur doit être lancé avant que le client ne commence à émettre)    

 - Reconstruisez tous les fichiers et chargez votre image dans la mémoire de la cible
 - Lancez l'application

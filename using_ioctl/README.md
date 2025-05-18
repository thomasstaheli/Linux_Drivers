# Rapport Thomas Stäheli
## Exercice 1

D'abord il faut trouver le couple majeur/mineur de `/dev/mem` :

```bash
$ ls -l /dev/random
```

Qui nous donne le résultat suivant :

```bash
crw-r----- 1 root kmem 1, 1 Jan  1 00:00 /dev/mem
```

Donc le numéro majeur = 1 (chiffre à gauche) et numéro mineur = 1 (chiffre à droite) 

Ensuite avec ces informations, on crée le fichier virtuel :

```bash
$ sudo mknod /tmp/myrandom c 1 1
$ sudo chmod 666 /tmp/myrandom # Même droit de lecture/écriture
```

### Qu'est-ce qui se passe lorsque vous lisez son contenu avec la commande cat ?

```bash
$ cat /tmp/myrandom
```

Une partie du résultat de la commande :

```bash
...
x���x8l`�``�v��|
                ��`lvff�0p000x



                              ��x�`flxl�p00000x����������x���x�ff|`�v��|
                                                                        �vf`�|�x
                                                                               �0|004����v���x0����l�l8l����|
                             ���0d�00�00�0000�v�8l������u�f`� �fa�`��ra�XM�fPR�@��f�lf@f=�rf3��pf�l�@
�t�Ȣ@u���$�����Q�ZfX�XMXMOracle VirtualBox BIOSXM�ω����������_SM_}�_DMI_S�
%XM�[��06/23/99�
```

On remarque que cela lit plein de données aléatoires à l'infini. Ce résultat arrive, car le l'alias `/tmp/myrandom` agit comme un alias de `/dev/random`, car il utilise le même couple majeur/mineur.


## Exercice 2

### Retrouvez cette information dans le fichier /proc/devices.

Pour retrouver cette information, on peut exécuter la commande suivante :

```bash
$ cat /proc/devices
```

Ce qui nous donne le résultat suivant :

```bash
Character devices:
....
 99 ppdev
108 ppp
128 ptm
136 pts
180 usb
188 ttyUSB
189 usb_device
202 cpu/msr
204 ttyMAX
226 drm
...

Block devices:
...
```

On voit bien que ttyUSB possède un majeur 188.

## Exercice 3

### Retrouvez-le dans l'arborescence de sysfs

Pour obtenir le driver, il faut chercher un peu, mais on peut déduire l'arborescence via le nom du périphérique. On sait qu'il faut chercher dans tty.

```bash
$ ls -l /sys/class/tty/ttyUSB0/device/driver
```

Résultat de la commande :

```bash
lrwxrwxrwx 1 root root 0 Apr  2 11:12 /sys/class/tty/ttyUSB0/device/driver -> ../../../../../../../bus/usb-serial/drivers/ftdi_sio
```

Le pilote utilisé est donc `ftdi_sio`.

### Vérifier avec la commande lsmod

On peut vérifier avec la commande `lsmod` :

```bash
$ lsmod | grep ftdi_sio
```

Résultat de la commande :

```bash
ftdi_sio               65536  1
usbserial              57344  3 ftdi_sio
```

On voit ici que `ftdi_sio` dépend du module `usbserial`, qui est un module plus générique

Pour chercher d'autres modules plus génériques, on peut utiliser la commande :

```bash
$ lsmod | grep usb
```

Et on obitient le résultat suivant :

```bash
usbhid                 65536  0
hid                   151552  2 usbhid,hid_generic
```

## Exercice 4

D'abord, on compile pour avoir le fichier .ko :

```bash
$ make
```

Ensuite, on le monte dans le noyau :

```bash
$ sudo insmod empty.ko
```

Ensuite, on démonte :

```bash
$ sudo rmmod empty
```

On réaffiche les logs :

```bash
$ dmesg -w
```

Résultat de la commande :

```bash
[ 5996.985543] Hello there!
[ 6278.854319] Good bye!
```

## Exercice 5

### Compilez et cross-compilez ce driver pour votre machine et pour la DE1-SoC. Vérifiez que le driver soit bien inséré sur les deux plates-formes et récupérez un maximum d'informations sur ce périphérique grâce aux outils précédemment vus.

Pour make, j'ai modifié le makefile, dans accumulate pour soit la compilation sur PC ou la compilation pour la DE1SOC : 

```bash
$ make 
$ sudo insmod accumulate.ko
$ cat /proc/devices | grep accumulate # Vérifier le majeur
 97 accumulate
$ dmesg -w
[ 8109.095334] ioctl ACCUMULATE_CMD_RESET: 11008
[ 8109.095335] ioctl ACCUMULATE_CMD_CHANGE_OP: 1074014977
```

Même chose pour la de1soc : 

```bash
# Modifier le make file
(HOST)$ make 
(HOST)$ cp accumulate.ko /export/drv
(DE1SOC)$ sudo insmod accumulate.ko
(DE1SOC)$ cat /proc/devices | grep accumulate # Vérifier le majeur
 97 accumulate
(DE1SOC)$ tail -f /var/log/kern.log
Jan  1 00:00:37 de1soclinux kernel: [   37.203012] Acumulate ready!
Jan  1 00:00:37 de1soclinux kernel: [   37.203023] ioctl ACCUMULATE_CMD_RESET: 11008
Jan  1 00:00:37 de1soclinux kernel: [   37.203032] ioctl ACCUMULATE_CMD_CHANGE_OP: 1074014977
```

-----

### Créez un device node afin de communiquer avec le driver (à choix sur votre machine ou sur la carte). Donnez les bons droits sur ce fichier afin que l'utilisateur courant puisse y accéder. Rendez un listing du device node. (c.-à-d. ls -la /dev/mynode).

J'ai décidé de faire ça sur la machine hôte. Pour créer le node on effectue les commandes suivantes :

```bash
$ sudo mknod /dev/accumulate c 97 0
$ sudo chmod 666 /dev/accumulate
```

Ensuite, on vérifie la création de notre device node, avec la commande :

```bash
$ ls -la /dev/accumulate
 
crw-rw-rw- 1 root root 97, 0 Apr  5 19:57 /dev/accumulate
```

-----

### Effectuez une écriture (echo) et une lecture (cat) sur ce device node. Grâce à ioctl.c, testez la configuration du périphérique, puis démontez-le. Vérifiez également que le démontage du noyau ait bien été effectué. Rendez une copie texte de votre console.

Pour faire une écriture, on traite notre driver comme un fichier :

```bash
$ echo 1 > /dev/accumulate
```

Pareil pour la lecture, on traite de la même manière que si on voulait lire un fcichier avec la commande cat :

```bash
$ cat /dev/accumulate
1
```

Ensuite, pour utiliser ioctl, je l'ai d'abord compilé avec gcc :

```bash
$ gcc ioctl.c -o ioctl
```

Ensuite, je pensais utiliser la commande suivante, mais j'ai obtenu une erreur :

```bash
$ ./ioctl /dev/accumulate ACCUMULATE_CMD_RESET 0
ioctl:: Invalid argument
```

Pour vérifier que les macros étaient existantes, j'ai tapé la commande suivante :

```bash
$ sudo dmesg | grep ACCUMULATE_CMD
[ 8109.095334] ioctl ACCUMULATE_CMD_RESET: 11008
[ 8109.095335] ioctl ACCUMULATE_CMD_CHANGE_OP: 1074014977
```

J'ai donc utilisé les valeurs en dur, et cela a fonctionné finalement, mais c'est pas très pratique :

```bash
$ ./ioctl /dev/accumulate 11008 0
$ cat /dev/accumulate
0
```

Ensuite, pour tester l'autre paramètre :
```bash
$ echo 5 > /dev/accumulate
$ ./ioctl /dev/accumulate 1074014977 1
$ echo 5 > /dev/accumulate
$ cat /dev/accumulate
25
```

Enfaite, finalement, il fallait simplement ajouter des includes.

Pour démonter le driver, il suffit de faire comme on a fait avant :

```bash
$ sudo rmmod accumulate
```

Puis, pour vérfier que le démontage c'est bien fait :

```bash
$ sudo dmesg -w

[18337.944031] Acumulate done!
```

### Tester le programme user-space

Pour compiler e et utiliser le programme de test :

```bash
$ gcc test_accumulate.c -o test_accumulate
$ ./test_accumulate
```
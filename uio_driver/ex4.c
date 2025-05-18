#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <stdint.h>

#define UIO_DEV "/dev/uio0"

#define FPGA_BASE_ADDR	      0xFF200000 // Addr des périphériques
#define HEX0_OFFSET	      0x20 // Offset HEX0, les leds commencent à 0xFF200020
#define HEX4_OFFSET_FROM_HEX0 0x10
#define LEDS_OFFSET	      0x00 // Offset LEDs, les leds commencent à 0xFF200000
#define KEY_OFFSET	      0x50 // Offset Boutons, les keys commencent à 
						       // 0xFF200050
// Interrupts OFFSET
#define KEY_OFFSET_INTERRUPTMASK_REGISTER 0x58  // Offset KEYS pour le registre
												// interruptmask
#define KEY_OFFSET_EDGECAPTURE_REGISTER   0x5C // Offset KEYS pour le registre 
												// edgecapture

#define PAGE_SIZE	      getpagesize() // Taille de la page mémoire
#define NB_HEX		      6 // Nombre d'affichage 7 segments
#define NB_HEX_FIRST_REG 4 // Nombre d'affichage 7 segments dans le premier 
						   // registre
#define NB_REGISTER_HEX 2 // Nombre de registre pour stocker l'état des 7 s
						  // segments

// Pointeur sur les différents périphériques
static volatile void *fpga_base;
static volatile uint32_t *hex[NB_HEX]; // Tableau de registre 32 bits
static volatile uint16_t *leds;
static volatile uint8_t *keys;
static volatile uint8_t *key_interrupt_mask;
static volatile uint8_t *key_edge_capture;

// Tableau de caractère
static const unsigned char hex_map[26] = {
	0x77, 0x7C, 0x39, 0x5E, 0x79, 0x71, 0x3D, 0x76, 0x06, 0x1E, // A-J
	0x75, 0x38, 0x37, 0x54, 0x5C, 0x73, 0x67, 0x50, 0x6D, 0x78, // K-T
	0x3E, 0x1C, 0x2A, 0x74, 0x6E, 0x5B // U-Z
};

/**
 * @brief Eteint les LEDS et HEX pour évite de faire fondre la banquise
 *
 * @param signum Numéro du signal reçu.
 *
 * ! Fonction écoresponsable !
 */
static void cleanup(int signum)
{
	// Eteint LED et HEX
	for(uint8_t i = 0; i < NB_REGISTER_HEX; ++i) {
        *hex[i] = 0x00;
    }
	*leds = 0x00;

	// Désalloue la page mémoire
	munmap((void *)fpga_base, PAGE_SIZE);
	exit(0);
}

/**
 * @brief Met à jour l'affichage des segments hexadécimaux en fonction du 
 * caractère affiché et du curseur.
 *
 * Cette fonction met à jour l'affichage des afficheurs 7 segments en 
 * fonction du caractère actuellement sélectionné dans `displayed_char`. 
 * Elle gère aussi l'affichage du curseur via les LEDs.
 *
 * @param displayed_char Pointeur vers la chaîne contenant les caractères 
 * à afficher.
 * @param cursor Position actuelle du curseur dans la chaîne `displayed_char`.
 *
 * La fonction détermine l'afficheur concerné et applique un masque pour 
 * modifier uniquement la partie du registre associée. Si le caractère à 
 * afficher est un espace, l'afficheur correspondant est éteint.
 */
static void update_display(char *displayed_char, uint8_t cursor)
{
	// Calcul de l'index pour savoir quel lettre il faut afficher
	uint8_t index;
	// Index registre pour les 7 segments soit (0 = hex[0] et 1 = hex[1])
	uint8_t index_reg_hex;
	// Masque permettant de modifier uniquement la partie désirée du 
	// registre
	uint8_t shift;

	// Si le curseur se trouve sur les hex 5 ou 6, si oui alors il faut $
	// modifier
	// le deuxième registre
	index_reg_hex = cursor > (NB_HEX_FIRST_REG - 1) ? 1 : 0;
	// Adapte le shift, selon le cursor et le index_reg_hex
	shift = (index_reg_hex ? cursor - NB_HEX_FIRST_REG : cursor);

	// index de la lettre dans le disctionnaire
	index = displayed_char[cursor] - 'A';

	// Mise des bits à 0 selon le HEX sélectionné
	*hex[index_reg_hex] &= ~(0x7F << 8 * shift);
	// Si on a une lettre a affiché
	if (displayed_char[cursor] != ' ') {
		// Affichage de la lettre, appart si on a un espace vide
		*hex[index_reg_hex] |= (hex_map[index] << 8 * shift);
	}

	// Affiche le nombre hex de la lettre sur les LEDs
	*leds = 1 << cursor;
}

/**
 * @brief Gère une interruption provenant des boutons poussoirs et met à jour 
 * l'affichage.
 *
 * Cette fonction est appelée lorsqu'une interruption est déclenchée par l'un
 * des boutons KEY0 à KEY3. Elle lit la valeur du registre d'interruption, 
 * détermine quel bouton a été pressé et effectue l'action correspondante :
 *   - **KEY0 (0x01)** : Décrémente le caractère affiché.
 *   - **KEY1 (0x02)** : Incrémente le caractère affiché.
 *   - **KEY2 (0x04)** : Déplace le curseur vers la droite.
 *   - **KEY3 (0x08)** : Déplace le curseur vers la gauche.
 *
 * Une fois l'action exécutée, la fonction met à jour l'affichage et 
 * réinitialise le registre de capture.
 *
 * @param displayed_char Pointeur vers la chaîne contenant les caractères 
 * affichés.
 * @param cursor Pointeur vers la position actuelle du curseur.
 */
static void handle_interrupt(char *displayed_char, uint8_t *cursor)
{
    // Lire KEY0, KEY1, KEY2, KEY3
	uint8_t key_val = *key_edge_capture & 0x0F; 

	// Si key1 alors on décrémente
	if (key_val & 0x01) {
		displayed_char[*cursor] = (displayed_char[*cursor] == 'A') ?
						' ' : (displayed_char[*cursor] == ' ') ?
						displayed_char[*cursor] : displayed_char[*cursor] - 1;
	}
	// Si key2 alors on incrémente
	if (key_val & 0x02) {
		displayed_char[*cursor] = (displayed_char[*cursor] == ' ') ?
						'A' : (displayed_char[*cursor] == 'Z') ? 
						displayed_char[*cursor] : displayed_char[*cursor] + 1;
	} 
	// Si key3 alors on déplace l'index vers la droite
	if (key_val & 0x04) {
		// Update du curseur
		*cursor = (*cursor == 5) ? 0 : *cursor + 1;
	}
	// Si key4 alors on désplace l'index vers la gauche
	if (key_val & 0x08) {
		// Update du curseur
		*cursor = (*cursor == 0) ? 5 : *cursor - 1;
	}
	// Update display
	update_display(displayed_char, *cursor);

    // Remise à 0 du registre de capture
    *key_edge_capture = 0xF;
}

int main()
{
	// Ouvrir le périphérique UIO
    int fd = open(UIO_DEV, O_RDWR);
    if (fd < 0) {
        perror("Erreur ouverture UIO");
        return EXIT_FAILURE;
    }

	// Mapper l'espace mémoire des périphériques
	fpga_base = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
			 fd, 0);
	if (fpga_base == MAP_FAILED) {
		perror("Erreur mmap");
		close(fd);
		return EXIT_FAILURE;
	}

	// Obtenir les pointeurs vers les registres
	// Premier tour de boucle : Premier registre HEX contenant   : 
	// HEX0, HEX1, HEX2, HEX3
	// Deuxième tour de boucle : Deuxième registre HEX contenant : 
	// HEX4, HEX5
    for(unsigned i = 0; i < NB_REGISTER_HEX; ++i) {
        hex[i] = (uint32_t *)(fpga_base + HEX0_OFFSET + 
								HEX4_OFFSET_FROM_HEX0 * i);
    }
	// Stocke les adresses pour les IOs : LEDS + KEYS
	leds = (uint16_t *)(fpga_base + LEDS_OFFSET);
	keys = (uint8_t *)(fpga_base + KEY_OFFSET);
	// Stocke les adresse pour la gestion des interruptions des keys
	key_interrupt_mask = (uint8_t *)(fpga_base + 
									KEY_OFFSET_INTERRUPTMASK_REGISTER);
    key_edge_capture = (uint8_t *)(fpga_base + 
									KEY_OFFSET_EDGECAPTURE_REGISTER);
	// Initialise les interruptions
    *key_interrupt_mask = 0xF;
    *key_edge_capture   = 0xF;

	// Bind CTRL+C avec la fonction cleanup
	signal(SIGINT, cleanup);

	// Tableau contenant toutes les lettres à afficher
	char displayed_char[NB_HEX];
	// Index curseur, pour savoir quel HEX on sélectionne
	uint8_t cursor = 0;

	// Initialisation de l'affichage avec des caractères 'A'
	for(uint8_t i = 0; i < NB_HEX; ++i) {
		displayed_char[i] = 'A';
		// Afficher 'A' sur tous les displays au démarage
		update_display(displayed_char, i);
	}
	// Remise du curseur à 0
	update_display(displayed_char, 0);

	int toggle_irq = 1;
	ssize_t nb;

	while (1) {
		// Reset les interruptions
		nb = write(fd, &toggle_irq, sizeof(toggle_irq));
        if (nb != (ssize_t)sizeof(toggle_irq)) {
            perror("write");
            close(fd);
            exit(EXIT_FAILURE);
        }

        // Attente d'interruption
        nb = read(fd, &toggle_irq, sizeof(toggle_irq));
        if (nb == (ssize_t)sizeof(toggle_irq)) {
			handle_interrupt(displayed_char, &cursor);
        }

	}

	cleanup(0);
	return 0;
}

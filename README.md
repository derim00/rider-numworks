# Rider – édition NumWorks

Portage du jeu Unity **« Rider Game »** (Brackeys) en application native
`.nwa` pour la calculatrice NumWorks.

Fichier à installer : **`Rider.nwa`** (≈ 440 Ko une fois installé, limite : 2 Mo).

fait part cjnw (yamii) discord.gg/sti2d
discord.gg/derim

## Installation

1. Branche la calculatrice en USB.
2. Ouvre <https://my.numworks.com/apps> dans Chrome ou Edge.
3. Choisis « Installer une application » et sélectionne `Rider.nwa`.
4. L'icône **Rider** apparaît dans le menu de la calculatrice.

Modèles compatibles : N0110, N0115, N0120 (Epsilon récent). Le N0100 ne
prend pas en charge les applications externes.

## Commandes

| Touche | Action |
| --- | --- |
| **OK**, **EXE** ou **↑** (maintenir) | au sol : accélérer · en l'air : rotation (flip) |
| **Retour** / **⌫** | pause (Continuer / Recommencer / Niveaux / Menu) |
| Flèches + **OK** | naviguer dans les menus |
| **shift** (menu) | aide |
| **Maison** | quitter |

Comme dans l'original, il n'y a qu'un seul bouton : maintiens-le pour rouler,
et en l'air la voiture tourne en arrière. Relâche pour atterrir sur les roues.

## Ce qui vient de l'original

- La physique de `CarController.cs` : force de 40 N au sol, couple de 14 en
  l'air, masse 5, frottement linéaire 0,3, angulaire 3, gravité 9,81,
  matériau sans friction. Le collider polygonal de la voiture est repris tel quel.
- Le niveau 1 « DESERT » reprend exactement le circuit de `Main.unity` : les 4
  courbes de Bézier jaunes et rouges, la position de départ, le drapeau, la
  ligne d'arrivée (x = 73,36) et la chute mortelle (y < -9,5).
- Les images de l'original : fond désertique, voiture, drapeau, bouton jaune
  et l'image « Done! ».
- La caméra qui suit la voiture avec un décalage, et l'arrêt du temps à
  l'arrivée (`Time.timeScale = 0`).

## Ajouts pour la calculatrice

- 8 nouveaux niveaux (collines, loopings, sauts, vagues, chute, double looping,
  ciel, final), tous validés par un bot de test à 15, 25, 40 et 60 images/s.
- Crash si le toit touche le sol, avec explosion de particules et
  redémarrage automatique.
- Compteur de flips (« DOUBLE FLIP ! »), chronomètre et record par niveau
  (gardé pendant la session).
- Menu principal, sélection de niveau, aide, pause et écran de fin.
- Lignes néon lissées (anticrénelage) avec halo, voiture tournée avec
  suréchantillonnage 2×2, poussière et étincelles, fond en défilement parallaxe.
- Rendu par bandes de 30 lignes : environ 46 Ko de RAM, sans scintillement.

## Compiler

Il faut `arm-none-eabi-gcc`, Node.js et Python 3 (Pillow + numpy).

```sh
make assets   # régénère src/assets.h et src/levels.h (optionnel, déjà fournis)
make build    # -> build/rider.nwa
make test     # bot de test sur PC : vérifie que les 9 niveaux sont faisables
```

Organisation du code :

- `src/game.c` : moteur (physique, rendu, menus), indépendant de la plateforme
- `src/main_eadk.c` : point d'entrée NumWorks (EADK)
- `tools/gen_assets.py` : convertit les images et polices
- `tools/gen_levels.py` : définit les circuits
- `host/host.c` : harnais PC (bot et captures d'écran)

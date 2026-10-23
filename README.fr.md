# FiloMiner

English version: [README.md](README.md)

Mineur GPU (NVIDIA, CUDA) pour **FiloPow (FPOW)** et les autres monnaies KawPow qui utilisent les
constantes de Ravencoin. Écrit pour et testé sur des cartes NVIDIA **P104-100** (Pascal, sm_61).

**Aucun frais de développeur.** Chaque share trouvé va à votre adresse. Apache-2.0.

Zip de la release Windows : https://github.com/gacanga/FiloMiner/releases

## Ce qu'il mine

- **Monnaie :** FiloPow (FPOW) — https://github.com/filoproject/filopow
- **Algorithme :** KawPow (ProgPoW 0.9.4 : période de 3 blocs, epoch de 7 500 blocs, items du DAG à
  512 parents), identique à celui de Ravencoin ; FiloPow a seulement renommé le tableau de constantes.
  Les autres monnaies KawPow aux constantes de Ravencoin devraient marcher aussi (seul FPOW a été testé).
- **Protocole de pool :** KawPow NOMP / kawpow-stratum
  (`mining.notify [job, header_hash, seed_hash, target, clean, height, nbits]`), TCP simple ou TLS.

## Fonctionnement

- Le DAG est construit sur le GPU à partir du light cache (une construction par epoch).
- Le kernel de recherche est **compilé à l'exécution par NVRTC** pour chaque période ProgPoW (tous
  les 3 blocs), et le kernel de la période suivante est compilé à l'avance en arrière-plan : le minage
  n'attend jamais le compilateur.
- Chaque candidat du GPU est **recalculé sur le CPU** (bibliothèque de référence ethash) avant d'être
  envoyé ; un écart est signalé comme erreur GPU (en général des fréquences instables) et le share
  n'est pas soumis.
- Chiens de garde : un GPU dont un lancement ne revient jamais fait quitter le mineur avec le code 4,
  une erreur CUDA avec le code 3, pour que le lanceur le relance.

## GPU pris en charge

NVIDIA Pascal (GTX 10xx, P104-100, P106-100) et plus récents, avec 4 Go de mémoire ou plus (le DAG de
FPOW fait environ 1,2 Go fin 2026 et grossit de 8 Mo tous les 5 jours). La partie fixe du code GPU est
compilée pour sm_61 + PTX (les cartes plus récentes la compilent à la volée) ; le kernel de recherche
est compilé par NVRTC pour la carte réelle. Il faut un pilote NVIDIA R5xx ou plus récent compatible
CUDA 12 (CUDA 12.9 est le dernier toolkit qui compile pour Pascal).

## Démarrage rapide (zip de la release Windows)

1. Télécharger le zip sur https://github.com/gacanga/FiloMiner/releases et le décompresser.
2. Modifier `filominer.conf` : remplacer `YOUR_FPOW_ADDRESS` sur la ligne `user` par votre adresse
   FPOW (commence par `F`), garder `.rig1` ou mettre n'importe quel nom de rig.
3. Facultatif : ouvrir une invite de commandes dans le dossier et lancer `filominer.exe --test` — la
   sortie doit finir par `TEST RESULT: ALL OK`.
4. Double-cliquer sur `start_fpow.bat` (relance le mineur 10 s après tout arrêt ; journal
   `filominer.log`).

Équivalent en ligne de commande :

```
filominer.exe -o stratum+tcp://pool.filopow.xyz:3333 -u YOUR_FPOW_ADDRESS.rig1 -p x
filominer.exe --test        # référence CPU + tests de justesse GPU
filominer.exe --bench 60    # benchmark de hashrate (60 s)
```

## Référence de configuration

`filominer.conf` est lu dans le dossier de `filominer.exe` (ou dans son dossier parent), ou dans le
fichier donné par `--config`. Chaque ligne est `clé = valeur` (le nom de l'option sans `--`) ; `#` ou
`;` commencent un commentaire. Les options de la ligne de commande priment sur le fichier ; `-o` en
ligne de commande remplace les pools du fichier.

| Option (CLI) | Clé de config | Défaut | Signification |
|---|---|---|---|
| `-o`, `--pool`, `--url URL` | `pool` / `url` / `o` | — | URL du pool, répétable (ordre de bascule). `stratum+tcp://` = TCP simple, `stratum+ssl://` / `stratum+tls://` (ou pas de `tcp` dans le schéma) = TLS |
| `-u`, `--user ADRESSE.RIG` | `user` / `u` | — | Adresse FPOW, un point, un nom de rig |
| `-p`, `--pass X` | `pass` / `p` | `x` | Mot de passe du pool |
| `--gpus 0,1\|all` | `gpus` / `devices` | `all` | GPU à utiliser (index CUDA ; les lanceurs mettent `CUDA_DEVICE_ORDER=PCI_BUS_ID`, soit l'ordre de `nvidia-smi`) |
| `--tpb N` | `tpb` | `256` | Threads par bloc (multiple de 32, 32..1024) |
| `--grid N` | `grid` | `0` | Blocs par lancement, `0` = automatique d'après `--target-ms` |
| `--target-ms N` | `target-ms` | `150` | Durée d'un lancement GPU pour la grille automatique |
| `--stats N` | `stats` | `30` | Secondes entre deux lignes de résumé (minimum 5) |
| `--log FICHIER` | `log` | — | Ajoute le journal à un fichier |
| `--cafile FICHIER` | `cafile` | voir plus bas | Certificats racine pour les pools TLS |
| `--bench-height N` | `bench-height` | `150000` | Hauteur de bloc (epoch / période) utilisée par `--bench`, `--nvrtc-check` |
| `--config FICHIER`, `-c FICHIER` | — | `filominer.conf` | Fichier de configuration |
| `--args-file FICHIER` | — | — | Lit d'autres arguments dans un fichier (séparés par des espaces) |
| `--test` | — | — | Référence CPU contre les vecteurs de test KawPow, puis sur chaque GPU choisi : items du DAG, hashs complets (mix + final) de 2 périodes, filtre de candidats du kernel de recherche |
| `--bench [s]` | — | 30 | Benchmark de hashrate |
| `--nvrtc-check` | — | — | Compile le kernel d'une période pour sm_61 sans GPU (vérifie les DLL NVRTC). **Source uniquement / prochaine release :** absent du binaire Windows v0.1.0 |
| `--help`, `-h` | — | — | Aide |

La vérification des certificats TLS utilise `ca-bundle.crt` à côté de l'exe s'il existe, sinon
`C:\msys64\mingw64\etc\ssl\certs\ca-bundle.crt` ou `/etc/ssl/certs/ca-certificates.crt`.

Variables d'environnement : `FM_NVRTC_OPTIONS` (options NVRTC en plus, ex. `--maxrregcount=72`),
`FM_DUMP_DIR` (garde dans ce dossier les sources de kernel générées et les cubins).

Codes de sortie : 2 = arguments invalides / pool ou user manquant, 3 = erreur CUDA, 4 = un GPU
n'avance plus (fréquences instables), 5 = délai de `--test` / `--bench` dépassé.

## Pool

Pool officiel **pool.filopow.xyz** (celui de `filominer.conf`) :

- `stratum+tcp://pool.filopow.xyz:3333` — difficulté de départ 0,1
- `stratum+tcp://pool.filopow.xyz:4444` — difficulté 1
- frais du pool 1 %, PROP, paiement toutes les 10 min, minimum 1 FPOW (conditions du pool au moment
  de l'écriture)

## Performances (mesurées)

NVIDIA P104-100, réglages par défaut (`tpb 256`, grille automatique) :

| | |
|---|---|
| Hashrate | environ **16 MH/s** par carte (réglages d'usine de la carte) |
| Construction du DAG au démarrage / nouvelle epoch | environ 13 s par GPU |
| Compilation NVRTC du kernel d'une période | environ 1,5 s (puis en arrière-plan, tous les 3 blocs) |

## Compilation

### Windows

Outils attendus (chemins par défaut des installeurs) :

- **CUDA 12.9** (`CUDA_PATH_V12_9`, sinon `C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.9`)
- **Visual Studio 2022** avec les outils C++ x64 (trouvé par `vswhere`) — compile `fmgpu.dll` avec nvcc
- **MSYS2** dans `C:\msys64`, environnement MINGW64, paquets `mingw-w64-x86_64-gcc` et
  `mingw-w64-x86_64-openssl` — compile `filominer.exe` (lié statiquement quand c'est possible)

```
build_all.bat            compile dev\ (fmgpu.dll, filominer.exe, DLL CUDA/NVRTC, ca-bundle.crt), puis lance les tests
build_all.bat nobuild    tests seulement
run_test.bat             tests de dev\filominer.exe        -> logs\test.log  ("TEST RESULT: ALL OK")
run_bench.bat            benchmark de 30 s de dev\         -> logs\bench.log
deploy.bat               dev\ -> bin\ (refusé si les derniers tests ne sont pas OK)
fpow_pool.bat            mine avec bin\filominer.exe et filominer.conf, relance auto, logs\pool.log
stop_fpow.bat            arrête fpow_pool.bat et filominer.exe
```

Les journaux vont dans `logs\`. Fichiers de réglage facultatifs dans `run\` : `nvcc_extra.txt`
(options nvcc en plus), `nvrtc_options.txt` (`FM_NVRTC_OPTIONS`), `test_args.txt`, `bench_args.txt`,
`pool_args.txt` (arguments du mineur en plus, ex. `--gpus 2`). `FM_HOST_ARCH` remplace
`-march=native` pour le programme hôte (builds de release : `-march=x86-64 -mtune=generic`) ;
`FM_CRT=/MT` compile la DLL avec le runtime C statique.

La source du kernel de recherche (`gpu/kawpow_kernel.cuh`) est intégrée à la DLL via le fichier généré
`gpu/kawpow_kernel_src.h` ; après une modification du kernel, lancer `python tools/embed_kernel.py`.

### Linux

```
bash linux/build_linux.sh        # -> build/filominer
bash linux/package_linux.sh      # -> release/FiloMiner-v<ver>-linux-x64-cuda12.tar.gz
```

Il faut le CUDA Toolkit 12.x (12.0 .. 12.9) avec NVRTC (`cuda-nvrtc-dev`), g++ (C++17), `libssl-dev`,
python3. Le runtime CUDA, NVRTC, OpenSSL (si `libssl.a` existe) et le runtime C++ sont liés
statiquement : le binaire n'a besoin que du pilote NVIDIA ; il tourne sur glibc 2.35+ (voir
`linux/glibc_compat.c`). Variables : `NVCC`, `CUDA_ARCH` (61 par défaut), `FM_HOST_ARCH`. Le paquet
Linux (`linux/dist/`) contient `start.sh`, `test.sh`, `bench.sh` et un service systemd facultatif avec
une garde GPU (`install-service.sh`).

## Tests

- `filominer --test` (ou `run_test.bat`, `linux/dist/test.sh`) : référence ethash contre les vecteurs
  de test KawPow, puis sur chaque GPU les items du DAG, les hashs complets de 2 périodes et le filtre
  de candidats, comparés au CPU.
- Vérifications sans GPU : `tests/ref_test.cpp` (référence contre les vecteurs de test, code des items
  du DAG contre la bibliothèque) et `tools/sim_test.sh` (exécute le kernel KawPow généré sur le CPU via
  `tests/sim_kernel.cpp` et compare 16 nonces par vecteur de test avec la référence).

## Organisation

```
src/          programme hôte : stratum, config, vérification CPU (kawpow_ref), TLS, JSON
gpu/          moteur CUDA (fmgpu.dll) : construction du DAG, générateur de programme ProgPoW, kernel NVRTC
third_party/  bibliothèque de référence ethash / ProgPoW (variante KawPow), Apache-2.0
windows/      scripts de compilation Windows (nvcc + MSVC pour la DLL, g++ MSYS2 pour l'exe)
linux/        scripts de compilation, de paquet et de release Linux
tests/ tools/ tests sans GPU, intégration du kernel et générateur de programme
dist/         fichiers non binaires du zip de la release Windows
```

## Licence

Licence Apache 2.0 — voir [LICENSE](LICENSE) et [NOTICE](NOTICE). `third_party/crypto/ethash` est
Copyright 2018-2019 Pawel Bylica, Apache-2.0 (repris des sources de FiloPow, qui le reprennent de
Ravencoin).

# Audit de la bibliothèque `uvpp-protocols`

Date : 2026-07-09  
Version auditée : `0.8.0`, branche `milestone-7`  
Nature de l'audit : architecture, qualité d'écriture, extensibilité, robustesse,
performance et outillage

## 1. Synthèse

`uvpp-protocols` est nettement au-dessus de la moyenne pour une bibliothèque
encore en version `0.8.0`. Le projet possède des intentions architecturales
claires, une API C++20 cohérente, un modèle d'ownership explicite, une très bonne
documentation et une couverture fonctionnelle sérieuse.

La bibliothèque n'est toutefois pas encore prête pour une exposition Internet
hostile, ni pour multiplier immédiatement les nouveaux protocoles. Le socle est
bon, mais plusieurs invariants annoncés par la conception ne sont pas encore
tenus par l'implémentation : le streaming HTTP serveur n'est pas réellement
borné, le client HTTP repose sur un parseur artisanal incomplet, certaines
sorties HTTP ne sont pas protégées contre l'injection de headers et le helper de
fichiers statiques bloque l'event loop.

Un jalon de consolidation ciblé est recommandé avant le client WebSocket, Redis,
SMTP ou d'autres familles de protocoles. Une réécriture générale ne paraît pas
nécessaire.

| Axe | Évaluation |
|---|---|
| Conception générale | Bonne |
| Lisibilité et style | Bonne, avec quelques unités de traduction trop concentrées |
| Tests et documentation | Très bonne |
| Extensibilité actuelle | Moyenne à bonne |
| Performance | Correcte pour de petits usages, avec plusieurs limites importantes à charge |
| Robustesse production | Insuffisante avant correction des constats prioritaires |

## 2. Périmètre et méthode

L'audit couvre :

- les headers publics sous `include/uvpp/protocols` ;
- les implémentations IO, URL, DNS, HTTP, TLS et WebSocket ;
- le routeur, les politiques de body, multipart, SSE et les fichiers statiques ;
- les ADR, documents de conception, propositions et roadmap ;
- CMake, le packaging, les exemples, le harnais de test et la CI.

Vérifications exécutées :

- compilation et tests complets avec GCC ;
- compilation et tests complets avec Clang ;
- compilation Clang avec `-Wall -Wextra -Wpedantic -Wconversion -Wshadow` ;
- compilation et exécution sous AddressSanitizer et UndefinedBehaviorSanitizer ;
- installation CMake dans un préfixe temporaire ;
- compilation et exécution d'un projet consommateur minimal depuis le package
  installé ;
- revue statique des chemins de parsing, ownership, timeout, backpressure et
  fermeture.

Les mesures réalisées ne constituent pas un benchmark de débit ou de latence.
Le projet ne contient actuellement aucun benchmark reproductible permettant de
chiffrer ses performances absolues.

## 3. Points forts

### 3.1 Composition explicite des transports

Le meilleur élément du design est la composition autour de
[`uvp::io::byte_stream`](../../include/uvpp/protocols/io/byte_stream.hpp) et
[`uvp::io::stream_listener`](../../include/uvpp/protocols/io/stream_listener.hpp).

TCP, pipes, TLS, HTTP upgrade et WebSocket se composent sans héritage entre les
protocoles. Le coût de la type erasure et des appels virtuels existe, mais il est
faible face aux opérations réseau, TLS et parsing, et achète une frontière de
composition très utile. Cette abstraction peut raisonnablement rester stable
pendant plusieurs jalons.

### 3.2 Ownership et event loop visibles

Les propriétaires asynchrones reçoivent explicitement un `uv::loop` ou un
transport. Les handles de session ou d'opération sont généralement move-only ou
portent un `shared_ptr` vers un état dont la durée de vie est visible. Le serveur
HTTP est volontairement non déplaçable une fois construit.

Cette approche est conforme à
[`ADR 0001`](../adr/0001-explicit-uvpp-loop-and-visible-ownership.md) et évite une
grande partie des ambiguïtés habituelles des frameworks asynchrones.

### 3.3 API HTTP expressive

Les politiques de body explicites (`none`, `bytes`, `text`, `json`, `stream`,
multipart) rendent la stratégie de buffering visible dans la déclaration de
route. Le routeur trie par segments évite un scan linéaire de toutes les routes
et possède des priorités stables entre routes statiques, paramètres et
wildcards.

Les réponses buffered, deferred et streaming expriment correctement trois
modèles de durée de vie distincts. La backpressure est visible à travers
`stream_write_result` au lieu d'être cachée derrière un simple booléen.

### 3.4 Documentation et discipline de conception

Le dépôt contient des ADR, des documents de design, des propositions séparées
des décisions stabilisées et des guides utilisateur. La proposition
[`shared-protocol-foundation.md`](../proposals/shared-protocol-foundation.md) est
particulièrement bien orientée : elle identifie les répétitions de completion,
timeout, cancellation, framing et backpressure sans proposer immédiatement un
moteur universel.

### 3.5 Couverture fonctionnelle

La suite contient 196 cas de test, dont de nombreux tests d'intégration réseau,
TLS, timeouts, limites, multipart, backpressure, keep-alive et fermeture. GCC et
Clang sont tous deux vérifiés en CI.

## 4. Constats prioritaires

### 4.1 Critique — Le streaming HTTP serveur conserve les bodies complets

Le parseur HTTP serveur ajoute chaque chunk au body complet :

```cpp
self.current_.body.append(at, length);
self.events_.push_back(http1_event::body(std::string{at, length}));
```

À la fin du message, il copie encore le message dans un événement puis déplace
une autre instance dans `completed_messages_` :

```cpp
self.events_.push_back(http1_event::complete(self.current_));
self.completed_messages_.push_back(std::move(self.current_));
```

Voir
[`src/http/detail/http1_state_machine.cpp`](../../src/http/detail/http1_state_machine.cpp).

Le serveur consomme ces événements en avançant seulement `handled_events_` dans
[`src/http/server.cpp`](../../src/http/server.cpp). Les événements et les
messages achevés ne sont pas drainés pendant la durée de la connexion.

Conséquences :

- `body::stream` ne borne pas réellement la mémoire au chunk courant ;
- plusieurs copies des données existent ;
- une connexion keep-alive conserve l'historique de ses requêtes ;
- des requêtes successives, chacune sous la limite individuelle, peuvent faire
  croître la mémoire sans limite ;
- multipart streaming hérite de la même rétention avant même son propre parsing.

Recommandation : rendre les événements consommables ou les envoyer directement
à la session, ne construire un body complet que pour les politiques buffered et
supprimer `completed_messages_` du chemin serveur de production.

### 4.2 Critique — Les headers sortants permettent l'injection CRLF

[`headers::set()` et `headers::add()`](../../src/http/headers.cpp) acceptent
n'importe quels noms et valeurs. Les sérialisations serveur et client les
écrivent directement sur le réseau :

- [`src/http/server.cpp`](../../src/http/server.cpp), dans
  `serialize_response_head()` ;
- [`src/http/client.cpp`](../../src/http/client.cpp), dans `write_request()`.

Une valeur contenant `\r\n` peut injecter un second header ou modifier le
framing HTTP. Le problème s'applique également au subprotocol WebSocket, inséré
directement dans la réponse de handshake.

Recommandation :

- valider les noms selon la grammaire HTTP `token` ;
- interdire au minimum CR, LF et NUL dans les valeurs ;
- valider les valeurs générant des headers spécialisés, notamment subprotocol,
  cache-control et proxy authorization ;
- ajouter des tests de response splitting et request smuggling sortant.

### 4.3 Élevé — Le client HTTP utilise un second parseur HTTP/1 artisanal

Le serveur utilise `llhttp`, mais le client parse manuellement status line,
headers, `Content-Length`, chunked encoding et trailers dans
[`src/http/client.cpp`](../../src/http/client.cpp).

Les variantes buffered et streaming dupliquent ensuite une partie importante de
la logique de framing. Cette duplication rend les corrections difficiles à
appliquer uniformément.

Un défaut concret concerne les réponses informationnelles. Les statuts `1xx`
sont considérés comme une réponse finale sans body et avec un framing
réutilisable. Une réponse `103 Early Hints` suivie d'un `200 OK` peut donc :

- achever prématurément la requête ;
- ignorer la réponse finale ;
- rendre au pool une connexion dont l'état applicatif n'est plus synchronisé.

Le parseur ne traite pas non plus rigoureusement :

- plusieurs `Content-Length` différents ;
- l'ambiguïté entre `Transfer-Encoding` et `Content-Length` ;
- toutes les contraintes de syntaxe de version et de status line ;
- les tokens exacts de `Transfer-Encoding` ;
- les réponses intermédiaires successives.

Recommandation : créer un adaptateur `llhttp` incrémental commun aux chemins
buffered et streaming. La machine de framing doit être unique ; seul le sink du
body doit varier.

### 4.4 Élevé — Défaut de lifetime possible avec un contexte TLS serveur temporaire

Le contexte OpenSSL installe un callback ALPN dont l'argument est un pointeur brut
vers `server_context::impl` :

```cpp
SSL_CTX_set_alpn_select_cb(ctx, select_alpn, this);
```

Voir [`src/tls/context.cpp`](../../src/tls/context.cpp).

`tls::accept()` reçoit le contexte par valeur mais ne le conserve pas dans
`tls_state` : voir [`src/tls/stream.cpp`](../../src/tls/stream.cpp). Avec un
contexte temporaire possédant son unique `impl`, le `SSL_CTX` peut rester vivant
par le référencement interne d'OpenSSL alors que l'objet `impl` pointé par le
callback a déjà été détruit.

Le design présente un second problème connexe : `server_context` et
`client_context` sont copiables par défaut et partagent un `shared_ptr<impl>`
mutable. Modifier une copie modifie donc les autres, sans que cette sémantique
soit documentée.

Recommandation :

- conserver explicitement l'état du contexte pendant tout handshake ;
- définir une sémantique claire : contexte move-only, copy-on-write, ou état
  immutable partagé après configuration ;
- empêcher ou documenter toute mutation après la première utilisation ;
- tester les contextes temporaires et les copies configurées différemment.

### 4.5 Élevé — La limite d'écriture HTTP n'est pas appliquée aux réponses buffered

Dans [`src/http/server.cpp`](../../src/http/server.cpp), `enqueue()` augmente
inconditionnellement `pending_write_bytes_` et ajoute la réponse dans la file.
`max_pending_write_bytes()` est consulté uniquement après l'ajout d'un chunk
streaming pour produire un signal de backpressure.

Le serveur autorise jusqu'à 16 slots de réponse par connexion par défaut. Des
réponses buffered très volumineuses peuvent donc être sérialisées et mises en
file, même lorsque la somme dépasse largement la limite annoncée.

Recommandation :

- appliquer une politique explicite aux réponses buffered : rejet, fermeture,
  file différée ou écriture segmentée ;
- inclure le framing dans la comptabilité ;
- vérifier les dépassements avant addition pour éviter les overflows ;
- tester plusieurs réponses pipelinées volumineuses.

### 4.6 Élevé pour la performance — Les fichiers statiques bloquent l'event loop

[`src/http/static_files.cpp`](../../src/http/static_files.cpp) utilise les API
synchrones suivantes depuis les callbacks du serveur :

- `std::filesystem::status()` ;
- `std::filesystem::file_size()` ;
- `std::filesystem::last_write_time()` ;
- construction de `std::ifstream` ;
- `std::ifstream::read()`.

Le body est envoyé par morceaux et la quantité mise en file reste bornée, mais
l'ouverture, les métadonnées et chaque lecture peuvent bloquer l'unique event
loop. Un stockage lent, un filesystem réseau ou de la contention disque peuvent
donc retarder toutes les connexions.

La proposition
[`static-file-helper.md`](../archive/static-file-helper.md) identifie déjà un
backend non bloquant comme travail différé. Cette limite doit rester très visible
dans la documentation utilisateur tant qu'elle existe.

Recommandation : utiliser le filesystem asynchrone de uvpp/libuv ou déléguer les
lectures à un backend injectable privé, sans changer l'API publique du handler.

### 4.7 Élevé — Conformité WebSocket encore incomplète

[`src/websocket/session.cpp`](../../src/websocket/session.cpp) vérifie bien le
masking client, les RSV bits, la fragmentation des control frames et certains
close codes. Il reste cependant plusieurs écarts :

- `ping()` et `pong()` permettent un payload supérieur à 125 octets ;
- `close()` permet une raison produisant une control frame supérieure à 125
  octets ;
- les messages texte entrants ne sont pas validés comme UTF-8 ;
- les raisons de fermeture entrantes et sortantes ne sont pas validées comme
  UTF-8 ;
- le subprotocol configuré n'est ni validé comme token, ni vérifié contre la
  liste proposée par le client ;
- la clé de handshake est seulement vérifiée comme non vide, pas comme un nonce
  base64 de 16 octets.

Recommandation : corriger ces invariants avant de partager la machine de framing
avec le futur client WebSocket.

## 5. Architecture et extensibilité

### 5.1 Frontière de transport solide

`byte_stream` est un bon point d'extension pour SMTP, Redis, MQTT, STARTTLS et
WebSocket. Il permet d'ajouter des protocoles sans faire dépendre leurs machines
d'état de TCP ou OpenSSL.

Cette partie ne demande pas de remise en question structurelle immédiate.

### 5.2 Packaging monolithique en contradiction avec les modules logiques

[`CMakeLists.txt`](../../CMakeLists.txt) compile toutes les sources dans un seul
target `uvpp_protocols` et lui attache llhttp, nlohmann/json et OpenSSL.

Conséquences :

- un consommateur URL ou DNS doit encore configurer des dépendances sans rapport
  avec son usage ;
- HTTP client dépend directement de TLS alors que les ADR décrivent une
  direction de dépendances plus stricte ;
- chaque nouveau module ajoutera ses dépendances au package entier ;
- les temps de configuration, compilation et distribution augmenteront ;
- la promesse de consommation indépendante des modules n'est pas tenue au niveau
  du package.

Le smoke test du package installé fonctionne, mais un projet ne consommant que
`uvp::url` déclenche tout de même la découverte d'OpenSSL.

Recommandation : séparer au minimum :

- `uvpp::protocols-core` ou des targets `io`, `url` et `dns` ;
- `uvpp::protocols-http` ;
- `uvpp::protocols-tls` ;
- `uvpp::protocols-websocket` ;
- un target agrégateur `uvpp::protocols` pour la compatibilité et la commodité.

### 5.3 Modèle de réponse client et serveur confondu

Le client retourne `result<http::response>`, mais `http::response` est aussi le
builder mutable utilisé par le serveur pour produire une réponse, la différer ou
la transformer en stream.

Cette réutilisation économise un type aujourd'hui, mais compliquera :

- les trailers ;
- les réponses informationnelles ;
- les upgrades ;
- la distinction body buffered/streamed ;
- HTTP/2 et ses streams indépendants ;
- l'immutabilité naturelle d'une réponse reçue.

Avant `1.0`, il serait préférable de distinguer un message ou une
`client_response` immutable du writer serveur.

### 5.4 Shared protocol foundation : bonne direction, extraction prudente

Les patterns suivants sont déjà répétés dans HTTP client, TCP connector, TLS et
WebSocket :

- completion exactly-once ;
- propagation de cancellation ;
- timers de phase ;
- fermeture sur erreur ;
- FIFO d'écritures ;
- comptabilité de bytes ;
- drain et backpressure.

La proposition de fondation partagée répond à un besoin réel. La bonne stratégie
est d'extraire de petits composants internes validés par plusieurs consommateurs :

- completion guard ;
- propriétaire de timer de phase ;
- bounded FIFO write queue ;
- helpers de parsing incrémental avec buffer et offset ;
- politique commune d'appel des callbacks utilisateur.

Il faut éviter un `protocol_session_base` ou une machine universelle imposant le
même lifecycle à HTTP, TLS, Redis et WebSocket.

## 6. Performance

### 6.1 Aspects favorables

- le routeur utilise un trie de segments ;
- les write queues conservent correctement les payloads jusqu'aux callbacks ;
- WebSocket utilise maintenant un offset de lecture et ne compacte plus le
  `vector` après chaque frame ;
- TLS borne les files cleartext en lecture et écriture ;
- multipart émet les données progressivement et conserve principalement une
  queue autour du délimiteur ;
- le type erasure des transports est un coût raisonnable pour ce domaine.

### 6.2 Coûts évitables

- rétention et copies multiples des bodies HTTP serveur ;
- reparsing complet du chunked body buffered à chaque nouvelle lecture client ;
- `std::string::erase(0, n)` répété dans le streaming client, entraînant des
  déplacements O(n) ;
- construction de plusieurs strings lors de la sérialisation d'une réponse
  buffered ;
- lookups de headers par scans linéaires ;
- filesystem synchrone ;
- callbacks stockés dans `std::function` sur tous les chemins, même les plus
  chauds.

Les scans de headers et `std::function` ne sont pas prioritaires : les limites de
mémoire, le parsing client et le filesystem bloquant dominent largement.

### 6.3 Benchmarks manquants

Avant toute promesse de performance, ajouter au moins :

- route matching avec 10, 100 et 1 000 routes ;
- parsing HTTP request/response avec bodies fragmentés ;
- keep-alive avec de nombreuses requêtes successives et suivi RSS ;
- upload/download streaming avec différentes tailles de chunks ;
- WebSocket petites frames et messages fragmentés ;
- TLS throughput et coût des petites écritures ;
- static files concurrents sur fichiers chauds et froids.

## 7. Lisibilité et maintenabilité

### 7.1 Qualité générale

Le code est généralement lisible : noms explicites, fonctions courtes dans les
types de valeur, invariants souvent contrôlés aux frontières et peu de magie
préprocesseur.

Les unités les plus difficiles à maintenir sont :

- `src/http/client.cpp`, environ 2 150 lignes ;
- `src/http/server.cpp`, environ 1 350 lignes ;
- `src/http/multipart.cpp`, environ 1 300 lignes ;
- `src/websocket/session.cpp`, environ 1 040 lignes ;
- `include/uvpp/protocols/http/router.hpp`, environ 1 120 lignes.

La taille n'est pas un défaut en soi, mais `http/client.cpp` contient deux
lifecycles complets et plusieurs parseurs. Le découper par machine d'état,
framing, pooling et sérialisation réduirait le risque de divergence.

Le header du routeur contient beaucoup d'overloads répétitifs. Cette répétition
améliore la visibilité IDE par rapport aux macros, mais augmente le bruit et le
temps de compilation. Une génération contrôlée ou des helpers privés peuvent
réduire ce coût sans réintroduire de macros publiques opaques.

### 7.2 Warnings stricts

Une compilation Clang avec warnings stricts a produit huit avertissements dans
le code de la bibliothèque :

- paramètres `policy` inutilisés dans le routeur ;
- constante multipart inutilisée ;
- paramètre OpenSSL inutilisé ;
- conversions signées/non signées ;
- une déclaration WebSocket masquant un membre.

Ils sont mineurs mais montrent que la CI ne compile pas encore avec une politique
de warnings explicite. Les warnings de llhttp doivent être isolés comme warnings
de dépendance tierce.

## 8. Tests, sanitizers et CI

### 8.1 Résultats

- GCC : 196/196 tests réussis ;
- Clang : 196/196 tests réussis ;
- tests HTTP, IO, multipart et TLS parcourus sous ASan/UBSan sans défaut détecté
  dans la bibliothèque avant l'arrêt du harnais ;
- tests WebSocket exécutés séparément sous ASan/UBSan : 7/7 réussis ;
- installation et consommation CMake minimale réussies.

### 8.2 Défaut du harnais sanitizer

ASan a détecté un `stack-use-after-scope` dans
[`tests/test.hpp`](../../tests/test.hpp). `UVP_CHECK_EQ` lie des références const
aux expressions comparées. Une expression telle que :

```cpp
UVP_CHECK_EQ(uvp::effective_port(url).value(), 443);
```

peut référencer le sous-objet d'un `optional` temporaire déjà détruit avant
l'appel à `check_equal()`.

Ce défaut appartient au harnais de test, pas au parseur URL. Il empêche néanmoins
une validation sanitizer totalement propre et doit être corrigé, par exemple en
capturant les expressions par valeur ou en appelant directement `check_equal`
dans la même full-expression.

### 8.3 Lacunes de CI

La CI devrait ajouter :

- warnings stricts sur le code projet ;
- ASan + UBSan ;
- éventuellement TSan sur un périmètre compatible ;
- smoke test `cmake --install` puis `find_package` depuis un projet externe ;
- fuzzing de llhttp adapter, URL, multipart et WebSocket frames ;
- test de consommation de chaque target modulaire ;
- mesure de croissance mémoire sur une connexion keep-alive longue.

## 9. Plan recommandé

### Étape 1 — Hardening immédiat

1. Valider tous les headers sortants.
2. Supprimer la rétention historique des bodies et événements HTTP serveur.
3. Corriger le lifetime des contextes TLS.
4. Faire respecter la limite de write queue aux réponses buffered.
5. Corriger les limites, UTF-8 et subprotocols WebSocket.
6. Corriger `UVP_CHECK_EQ` et rendre ASan/UBSan obligatoire.

### Étape 2 — Consolidation HTTP client

1. Remplacer le parseur manuel par un adaptateur `llhttp` response incrémental.
2. Unifier les chemins buffered et streaming autour de la même machine de
   framing.
3. Ajouter `1xx`, framings ambigus, trailers et limites de header count.
4. Remplacer les `erase(0, n)` répétés par un offset/compaction contrôlée.
5. Clarifier les timeouts idle versus deadlines totales.

### Étape 3 — Frontières de package

1. Séparer core/IO, URL/DNS, HTTP, TLS et WebSocket en targets.
2. Déclarer les dépendances CMake sur le target consommateur réel.
3. Conserver un target agrégateur pour la simplicité.
4. Ajouter des tests d'installation par module.

### Étape 4 — Shared protocol foundation

1. Extraire completion guard, timer owner et bounded write queue.
2. Valider ces helpers avec au moins HTTP client, WebSocket client et un petit
   protocole de service.
3. Garder les machines de protocole et leurs politiques dans leurs modules.

### Étape 5 — Expansion

Le client WebSocket devient alors un bon premier consommateur de la fondation.
Redis ou SMTP peuvent suivre afin de valider respectivement le multiplexage
simple/pipelining et les protocoles ligne + STARTTLS.

## 10. Conclusion

La bibliothèque possède une bonne architecture conceptuelle et une qualité
d'écriture globale sérieuse. Les choix les plus structurants — event loop
explicite, ownership visible, composition par byte streams, backends privés et
politiques de body — sont bien fondés.

Elle ne permettra cependant pas d'ajouter indéfiniment des fonctionnalités
élégamment « sans se remettre en question ». Aucun design sain ne peut réellement
garantir cela, et plusieurs points doivent être consolidés maintenant : parsing
client, rétention serveur, validation des sorties, sémantique des contextes TLS,
modularité CMake et filesystem asynchrone.

Ces corrections restent localisées. Elles demandent une remise à plat de quelques
internals et probablement une séparation du type de réponse client avant `1.0`,
mais pas une reconstruction de la bibliothèque. Après ce jalon, le projet pourra
accueillir de nouveaux protocoles sur un socle nettement plus durable.

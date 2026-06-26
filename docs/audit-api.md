# Audit API — uvpp-protocols
_(Temporary working file - sorry for the French)_

Cet audit examine l'API publique du projet telle qu'elle existe au milestone 3.
Il se place dans la perspective d'une reconstruction sans contrainte de
compatibilité descendante, afin de dégager les points qui auraient été conçus
différemment et les axes d'amélioration.

---

## 1. `server_options` et `accept_options` : interface publique unique

**Fichiers :** `include/uvpp/protocols/http/server_options.hpp`,
`include/uvpp/protocols/websocket/session.hpp`

Ce point a été corrigé : les champs de stockage (ex. `max_header_bytes_`,
`keep_alive_`, `on_text_`) sont désormais privés, et les setters fluents sont
la seule interface publique d'écriture.

La lecture publique passe par des accesseurs const sans préfixe `get_`, ce qui
garde le même nom canonique que le setter :

```cpp
auto options = uvp::http::server_options{}
  .max_header_bytes(32 * 1024)
  .server_header(false);

assert(options.max_header_bytes() == 32 * 1024);
assert(!options.server_header());
```

Le compromis retenu est donc : options comme value objects, écriture par
setters fluents, lecture par accesseurs homonymes, stockage privé avec suffixe
underscore.

---

## 2. `connection_info` : métadonnées de connexion sans accès transport

**Fichier :** `include/uvpp/protocols/http/connection.hpp`

Ce point a été corrigé : `request::connection()` n'expose plus de pointeurs
vers le transport. Le type public est désormais une vue immutable de
métadonnées :

```cpp
class connection_info {
public:
  const uvp::io::endpoint& local_endpoint() const noexcept;
  const uvp::io::endpoint& remote_endpoint() const noexcept;
};
```

Les handlers HTTP normaux peuvent inspecter les endpoints local et distant,
mais ne peuvent plus lire, écrire, fermer, ni spécialiser directement le
transport sous-jacent. La prise de possession du `uvp::io::byte_stream` reste
réservée au chemin explicite d'upgrade via `upgrade_request::accept()`.

---

## 3. `response::json` basé sur `uvp::json`

**Fichier :** `include/uvpp/protocols/http/response.hpp`

Ce point a été corrigé : l'ancienne surcharge
`initializer_list<pair<string_view, string_view>>` a été supprimée. Le projet
expose maintenant `uvp::json`, alias public de `nlohmann::json`, et
`response::json` accepte soit du JSON déjà sérialisé, soit une vraie valeur
JSON :

```cpp
void json(std::string_view serialized_json);
void json(const uvp::json& value);
```

```cpp
res.json(uvp::json{{"count", 42}, {"active", true}, {"meta", nullptr}});
```

Cela prépare aussi les futures routes à body typé JSON (`body::json<T>`) en
utilisant le même type de valeur et les conversions `from_json` / `to_json`.

---

## 4. Durée de vie implicite de la session WebSocket

**Fichier :** `include/uvpp/protocols/websocket/session.hpp`,
`examples/websocket_echo.cpp`

```cpp
uvp::websocket::accept_detached(req, uvp::websocket::accept_options{}.on_text(...));
```

**Résolu :** `uvp::websocket::accept()` retourne maintenant une session
`[[nodiscard]]` propriétaire. Le code applicatif doit conserver ce handle,
le déplacer vers un protocole supérieur, ou le convertir explicitement en
`byte_stream`.

Le comportement auto-entretenu via `shared_ptr` existe toujours pour les
endpoints callback-only, mais il est opt-in via
`uvp::websocket::accept_detached()`. L'exemple echo utilise cette API détachée
afin que la propriété de durée de vie soit visible au point d'appel.

---

## 5. `route_handler_type` et `body_mode` exposés dans l'API publique

**Fichier :** `include/uvpp/protocols/http/router.hpp`

```cpp
// Namespace uvp::http::detail — détails de dispatch interne
using route_handler_type = std::function<void(request&, response&,
    std::span<const std::byte>, request_body_stream*)>;

enum class body_mode { none, bytes, text, stream };
```

**Résolu :** ces types ont été déplacés dans `uvp::http::detail`.

Ils restent utilisés par `router` et `server` pour le dispatch interne, mais
ne sont plus des noms directs de l'espace `uvp::http`. Un utilisateur continue
à passer des lambdas typés à `srv.get(...)`, `srv.post(...)`, etc., sans écrire
de `route_handler_type` ni de `body_mode`.

---

## 6. Logique de routing inline dans le header public

**Fichiers :** `include/uvpp/protocols/http/router.hpp`,
`src/http/router.cpp`

**Résolu :** la logique de matching runtime a été déplacée dans
`src/http/router.cpp`.

`next_route_segment` est maintenant une fonction locale au `.cpp`.
`route_pattern_matches` est déclaré dans le header interne
`src/http/detail/route_matching.hpp`, partagé par `router.cpp` et
`server.cpp` pour les routes d'upgrade. Le header public conserve seulement
les templates nécessaires à l'enregistrement ergonomique des handlers.

---

## 7. Router en O(n) par scan linéaire

**Fichiers :** `include/uvpp/protocols/http/router.hpp`,
`src/http/router.cpp`

**Résolu :** le routeur HTTP principal utilise maintenant un trie de segments.

Les routes sont indexées par segment, avec des handlers par méthode stockés aux
nœuds terminaux. Le matching n'est plus proportionnel au nombre total de routes,
mais au nombre de segments du chemin, avec priorité stable :

1. segment statique ;
2. segment paramétré ;
3. wildcard final.

Le changement ajoute aussi la validation des patterns au moment de
l'enregistrement : paramètres nommés, wildcard final nommé, doublons et
conflits de paramètres.

---

## 8. Macros pour générer les méthodes HTTP

**Fichiers :** `include/uvpp/protocols/http/server.hpp`,
`include/uvpp/protocols/http/router.hpp`

```cpp
#define UVP_HTTP_SERVER_ROUTE_METHOD(name) \
  template<class BodyPolicy, class Handler> \
  server& name(...) { ... }

UVP_HTTP_SERVER_ROUTE_METHOD(get)
UVP_HTTP_SERVER_ROUTE_METHOD(post)
// ...
```

Les macros de génération de méthodes sont opaques aux IDEs, aux outils
d'analyse statique et aux générateurs de documentation. En C++17/20, une
approche tag-dispatch ou des helpers `constexpr` permettraient d'obtenir le
même résultat sans recourir au préprocesseur.

**Statut : résolu.** Les macros ont été remplacées par des méthodes explicites
dans `server` et `router`. Chaque verbe (`get`, `post`, `put`, `patch`,
`delete_`, `head`, `options`) expose maintenant directement ses deux overloads
publics, ce qui rend les signatures visibles aux IDEs, aux analyseurs statiques
et aux générateurs de documentation.

---

## 9. Callbacks dans `accept_options` WebSocket

**Fichier :** `include/uvpp/protocols/websocket/session.hpp`

`accept_options` mélange deux natures distinctes :

- **Configuration :** `max_message_bytes`, `max_pending_write_bytes`,
  `subprotocol`, `auto_pong`
- **Comportement :** `on_text`, `on_binary`, `on_ping`, `on_pong`,
  `on_close`, `on_error`

La configuration est inerte (valeurs scalaires) ; le comportement est un
ensemble de callbacks qui capturent de l'état applicatif. Les regrouper dans
un même type rend l'objet difficile à séparer (stocker les options sans les
handlers, changer un handler sans recréer les options, etc.).

**En zéro-compatibilité :** un type `session_config` pour les scalaires, et
un type `session_handler` (ou des setters sur la session elle-même) pour les
callbacks.

---

## 10. `status` enum incomplet

**Fichier :** `include/uvpp/protocols/http/status.hpp`

```cpp
enum class status : unsigned int {
  ok = 200, created = 201, no_content = 204,
  bad_request = 400, not_found = 404, method_not_allowed = 405,
  payload_too_large = 413, internal_server_error = 500, not_implemented = 501,
};
```

Neuf valeurs seulement. Les codes courants absents : `301`, `302`, `304`,
`401`, `403`, `408`, `409`, `422`, `429`, `502`, `503`. L'utilisateur est
contraint de passer par l'entier brut (`res.status(429)`) pour des cas
d'usage tout à fait ordinaires.

**Statut : résolu.** L'enum expose maintenant les codes courants manquants :
`moved_permanently`, `found`, `not_modified`, `unauthorized`, `forbidden`,
`request_timeout`, `conflict`, `unprocessable_content`,
`unprocessable_entity` (alias historique de `unprocessable_content`),
`too_many_requests`, `bad_gateway` et `service_unavailable`. Les reason phrases
associées sont également couvertes.

---

## 11. Pas de parsing des query parameters

**Fichier :** `include/uvpp/protocols/http/request.hpp`

```cpp
[[nodiscard]] std::string_view query() const noexcept { return query_; }
```

`req.query()` retourne la chaîne brute (`"foo=bar&baz=1"`). Il n'existe pas
de `req.query("foo")` pour accéder directement à un paramètre. L'utilisateur
doit parser lui-même, ce qui est une lacune ergonomique pour une API HTTP.

**Statut : résolu.** `request` conserve `query()` pour la chaîne brute et
expose maintenant une vue structurée immuable :

```cpp
std::optional<std::string_view> req.query("foo");
std::string_view req.query_or("foo", "fallback");
std::span<const std::string> req.query_all("tag");
const uvp::http::query_params& params = req.query_params();
```

Le parsing conserve les clés répétées, distingue absence et valeur vide, décode
les échappements `%XX`, traite `+` comme un espace et laisse les échappements
invalides sous forme littérale.

---

## 12. Pas de middleware ni de groupes de routes

L'API ne propose pas de concept de middleware (`srv.use(handler)`) ni de
groupes de routes préfixées (`srv.group("/api/v1", ...)`). Chaque route doit
répliquer indépendamment la logique transversale (authentification, CORS,
logging, validation de contenu).

Ce point est moins critique pour une lib bas-niveau axée sur la composition
explicite, mais constitue un point d'attention si l'objectif inclut
l'ergonomie applicative.

---

## 13. Couverture de tests insuffisante

**Fichier :** `tests/`

Le seul fichier de test vérifie essentiellement la compilation et quelques
assertions de structure (`assert`). Manquent notamment :

- Tests unitaires du parser HTTP (requêtes malformées, chunked encoding,
  headers en plusieurs passes)
- Tests du routing (collisions de patterns, wildcards, segments vides,
  méthode non trouvée vs. route trouvée mauvaise méthode)
- Tests du cycle de vie des réponses (`deferred`, `streaming`, `cancel`,
  double `end`)
- Tests d'intégration réseau réels (client-serveur sur socket local)

**Statut : résolu pour le socle initial.** Le test monolithique
`tests/structure_test.cpp` a été remplacé par un runner léger sans dépendance
externe et par des fichiers de tests spécialisés :

- `tests/http1_parser_test.cpp`
- `tests/http_request_test.cpp`
- `tests/http_router_test.cpp`
- `tests/http_response_test.cpp`
- `tests/http_server_integration_test.cpp`
- `tests/io_test.cpp`
- `tests/websocket_test.cpp`

La couverture inclut désormais le parser HTTP/1 (requête complète, parsing en
plusieurs passes, événements, chunked, erreur, upgrade), le routeur (priorités,
collisions, patterns invalides, body policies), les query parameters, les
cycles de vie de réponses principales et un test d'intégration TCP local avec
un vrai `uvp::http::server`.

---

## Résumé priorisé

| Priorité | Problème |
|---|---|
| ✅ Résolu | Session WebSocket : `accept()` propriétaire, `accept_detached()` explicite |
| ✅ Résolu | `route_handler_type`, `body_mode` déplacés dans `uvp::http::detail` |
| ✅ Résolu | Logique de routing runtime déplacée dans `src/http/router.cpp` |
| ✅ Résolu | Query parameters structurés sur `request` |
| ✅ Résolu | Socle de tests insuffisant remplacé par une suite structurée |
| ✅ Résolu | Router par trie de segments |
| ✅ Résolu | `status` enum enrichi avec les codes courants |
| ✅ Résolu | Méthodes HTTP explicites, sans macros de génération |
| 🟡 Conception | Callbacks et config mélangés dans `accept_options` |
| 🟡 Ergonomie | Pas de middleware ni de groupes de routes |

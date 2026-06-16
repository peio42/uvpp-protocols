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

## 2. `connection` : union informelle de raw pointers

**Fichier :** `include/uvpp/protocols/http/connection.hpp`

```cpp
class connection {
  uv::tcp* tcp_ = nullptr;
  uvp::io::byte_stream* stream_ = nullptr;
};
```

Les deux pointeurs peuvent être nuls simultanément, ou seulement l'un des
deux est non nul. C'est un `std::variant` déguisé sans ses garanties
d'exclusivité ni ses outils de dispatch. `request` expose ce type via
`req.connection()`, faisant fuir `uv::tcp*` dans l'API utilisateur — ce que
les principes de conception interdisent explicitement (« no implicit conversion
from protocol objects to raw libuv pointers »).

**En zéro-compatibilité :** `std::variant<uv::tcp*, uvp::io::byte_stream*>`,
ou simplement `byte_stream*` seul puisque c'est l'abstraction cible du projet.

---

## 3. `response::json` limité aux valeurs string

**Fichier :** `include/uvpp/protocols/http/response.hpp`

```cpp
void json(std::initializer_list<std::pair<std::string_view, std::string_view>> object);
```

La surcharge utilitaire `json(initializer_list<...>)` ne supporte que des
valeurs de type chaîne. Il est impossible de représenter un nombre, un
booléen, null ou un objet imbriqué. Le nom `json` induit en erreur sur la
portée réelle de la fonction.

```cpp
// Impossible, mais l'API laisse croire le contraire :
res.json({{"count", 42}, {"active", true}});
```

**En zéro-compatibilité :** supprimer cette surcharge ou la renommer
explicitement (`json_string_object`). Laisser à l'utilisateur le soin de
sérialiser via une lib JSON, et n'accepter que `std::string_view serialized`.

---

## 4. Durée de vie implicite de la session WebSocket

**Fichier :** `include/uvpp/protocols/websocket/session.hpp`,
`examples/websocket_echo.cpp`

```cpp
(void)uvp::websocket::accept(req, uvp::websocket::accept_options{}.on_text(...));
```

La session est détruite immédiatement après sa création, mais les callbacks
continuent de fonctionner grâce au `shared_ptr` interne. Ce comportement
viole le principe de moindre étonnement : une valeur de retour que l'on peut
ignorer sans conséquence visible masque une propriété importante de la durée
de vie de l'objet.

**En zéro-compatibilité :** la session doit être capturée explicitement
(`[[nodiscard]]` sur `accept`), ou la documentation doit rendre le contrat de
durée de vie immédiatement évident. L'auto-entretien via `shared_ptr` devrait
être opt-in, pas le comportement par défaut silencieux.

---

## 5. `route_handler_type` et `body_mode` exposés dans l'API publique

**Fichier :** `include/uvpp/protocols/http/router.hpp`

```cpp
// Namespace uvp::http — donc API publique
using route_handler_type = std::function<void(request&, response&,
    std::span<const std::byte>, request_body_stream*)>;

enum class body_mode { none, bytes, text, stream };
```

Ces types sont des détails du système de dispatch interne. Un utilisateur n'a
jamais besoin d'écrire un `route_handler_type` à la main : il passe un lambda
typé à `srv.get(...)`. L'exposition de `body_mode` dans l'espace de noms
public encourage un usage qui ne devrait pas exister.

**En zéro-compatibilité :** déplacer dans `uvp::http::detail::`.

---

## 6. Logique de routing inline dans le header public

**Fichier :** `include/uvpp/protocols/http/router.hpp`

Les fonctions `next_route_segment` et `route_pattern_matches` sont
`inline` dans le header, dans `uvp::http::detail`. Elles contiennent
une logique non triviale (segments, wildcards, paramètres nommés) qui
sera compilée dans chaque unité de traduction incluant `router.hpp`.

**En zéro-compatibilité :** les déplacer dans un `.cpp`. Le header ne
devrait exposer que les déclarations.

---

## 7. Router en O(n) par scan linéaire

**Fichier :** `include/uvpp/protocols/http/router.hpp`

```cpp
for (const auto& route_entry : routes_) { ... }
```

Toutes les routes sont comparées séquentiellement pour chaque requête.
Pour un usage en production avec un grand nombre de routes, un trie de
segments ou une table de dispatch par méthode + préfixe serait plus adapté.

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

---

## 11. Pas de parsing des query parameters

**Fichier :** `include/uvpp/protocols/http/request.hpp`

```cpp
[[nodiscard]] std::string_view query() const noexcept { return query_; }
```

`req.query()` retourne la chaîne brute (`"foo=bar&baz=1"`). Il n'existe pas
de `req.query("foo")` pour accéder directement à un paramètre. L'utilisateur
doit parser lui-même, ce qui est une lacune ergonomique pour une API HTTP.

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

**Fichier :** `tests/structure_test.cpp`

Le seul fichier de test vérifie essentiellement la compilation et quelques
assertions de structure (`assert`). Manquent notamment :

- Tests unitaires du parser HTTP (requêtes malformées, chunked encoding,
  headers en plusieurs passes)
- Tests du routing (collisions de patterns, wildcards, segments vides,
  méthode non trouvée vs. route trouvée mauvaise méthode)
- Tests du cycle de vie des réponses (`deferred`, `streaming`, `cancel`,
  double `end`)
- Tests d'intégration réseau réels (client-serveur sur socket local)

---

## Résumé priorisé

| Priorité | Problème |
|---|---|
| 🔴 API confuse | `connection` avec raw pointers nullables exposés dans `request` |
| 🔴 API trompeuse | `json()` limité aux string values |
| 🟠 Surprise runtime | Session WebSocket à durée de vie implicite |
| 🟠 Fuite d'implémentation | `route_handler_type`, `body_mode` dans le namespace public |
| 🟠 Build time | Logique de routing non triviale inline dans le header |
| 🟠 Ergonomie | Pas de parsing des query parameters |
| 🟠 Couverture | Tests insuffisants |
| 🟡 Performance | Router O(n) |
| 🟡 Exhaustivité | `status` enum incomplet |
| 🟡 Maintenabilité | Macros pour générer les méthodes HTTP |
| 🟡 Conception | Callbacks et config mélangés dans `accept_options` |
| 🟡 Ergonomie | Pas de middleware ni de groupes de routes |

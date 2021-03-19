open SharedTypes;
open Infix;

/* TODO should I hang on to location? */
let rec findDocAttribute = attributes => {
  Parsetree.(
    switch (attributes) {
    | [] => None
    | [
        (
          {Asttypes.txt: "ocaml.doc"},
          PStr([
            {
              pstr_desc:
                Pstr_eval(
                  {pexp_desc: Pexp_constant(Pconst_string(doc, _))},
                  _,
                ),
            },
          ]),
        ),
        ..._,
      ] =>
      Some(PrepareUtils.cleanOffStars(doc))
    | [_, ...rest] => findDocAttribute(rest)
    }
  );
};

let rec findDeprecatedAttribute = attributes => {
  Parsetree.(
    switch (attributes) {
    | [] => None
    | [
        (
          {Asttypes.txt: "deprecated"},
          PStr([
            {
              pstr_desc:
                Pstr_eval(
                  {pexp_desc: Pexp_constant(Pconst_string(msg, _))},
                  _,
                ),
            },
          ]),
        ),
        ..._,
      ] =>
      Some(msg)
    | [({Asttypes.txt: "deprecated"}, _), ..._] => Some("")
    | [_, ...rest] => findDeprecatedAttribute(rest)
    }
  );
};

let newDeclared =
    (
      ~item,
      ~scope,
      ~extent,
      ~name,
      ~stamp,
      ~modulePath,
      ~processDoc,
      exported,
      attributes,
    ) => {
  {
    name,
    stamp,
    extentLoc: extent,
    scopeLoc: scope,
    exported,
    modulePath,
    deprecated: findDeprecatedAttribute(attributes),
    docstring: findDocAttribute(attributes) |?>> processDoc,
    item,
    /* scopeType: Let, */
    /* scopeStart: env.scopeStart, */
  };
};

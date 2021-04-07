open SharedTypes;

type queryEnv = {
  file,
  exported,
};

let fileEnv = file => {file, exported: file.contents.exported};

let tupleOfLexing = ({Lexing.pos_lnum, pos_cnum, pos_bol}) => (
  pos_lnum - 1,
  pos_cnum - pos_bol,
);
let locationIsBefore = ({Location.loc_start}, pos) =>
  tupleOfLexing(loc_start) <= pos;

let findInScope = (pos, name, stamps) => {
  /* Log.log("Find " ++ name ++ " with " ++ string_of_int(Hashtbl.length(stamps)) ++ " stamps"); */
  Hashtbl.fold(
    (_stamp, declared, result) =>
      if (declared.name.txt == name) {
        /* Log.log("a stamp " ++ Utils.showLocation(declared.scopeLoc) ++ " " ++ string_of_int(l) ++ "," ++ string_of_int(c)); */
        if (locationIsBefore(declared.scopeLoc, pos)) {
          switch (result) {
          | None => Some(declared)
          | Some(current) =>
            if (current.name.loc.loc_start.pos_cnum
                < declared.name.loc.loc_start.pos_cnum) {
              Some(declared);
            } else {
              result;
            }
          };
        } else {
          result;
        };
      } else {
        /* Log.log("wrong name " ++ declared.name.txt); */
        result;
      },
    stamps,
    None,
  );
};

let rec joinPaths = (modulePath, path) => {
  switch (modulePath) {
  | Path.Pident(ident) => (ident.stamp, ident.name, path)
  | Papply(fnPath, _argPath) => joinPaths(fnPath, path)
  | Pdot(inner, name, _) => joinPaths(inner, Nested(name, path))
  };
};

let rec makePath = modulePath => {
  switch (modulePath) {
  | Path.Pident(ident) when ident.stamp === 0 => `GlobalMod(ident.name)
  | Pident(ident) => `Stamp(ident.stamp)
  | Papply(fnPath, _argPath) => makePath(fnPath)
  | Pdot(inner, name, _) => `Path(joinPaths(inner, Tip(name)))
  };
};

let makeRelativePath = (basePath, otherPath) => {
  let rec loop = (base, other, tip) =>
    if (Path.same(base, other)) {
      Some(tip);
    } else {
      switch (other) {
      | Pdot(inner, name, _) => loop(basePath, inner, Nested(name, tip))
      | _ => None
      };
    };
  switch (otherPath) {
  | Path.Pdot(inner, name, _) => loop(basePath, inner, Tip(name))
  | _ => None
  };
};

let rec resolvePathInner = (~env, ~path) => {
  switch (path) {
  | Tip(name) => Some(`Local((env, name)))
  | Nested(subName, subPath) =>
    switch (Hashtbl.find_opt(env.exported.modules, subName)) {
    | None => None
    | Some(stamp) =>
      switch (Hashtbl.find_opt(env.file.stamps.modules, stamp)) {
      | None => None
      | Some({item: kind}) => findInModule(~env, kind, subPath)
      }
    }
  }
}
and findInModule = (~env, kind, path) => {
  switch (kind) {
  | Structure({exported}) =>
    resolvePathInner(~env={...env, exported}, ~path)
  | Ident(modulePath) =>
    let (stamp, moduleName, fullPath) = joinPaths(modulePath, path);
    if (stamp == 0) {
      Some(`Global((moduleName, fullPath)));
    } else {
      switch (Hashtbl.find_opt(env.file.stamps.modules, stamp)) {
      | None => None
      | Some({item: kind}) => findInModule(~env, kind, fullPath)
      };
    };
  };
};

/* let rec findSubModule = (~env, ~getModule) */

let rec resolvePath = (~env, ~path, ~getModule) => {
  switch (resolvePathInner(~env, ~path)) {
  | None => None
  | Some(result) =>
    switch (result) {
    | `Local(env, name) => Some((env, name))
    | `Global(moduleName, fullPath) =>
      switch (getModule(moduleName)) {
      | None => None
      | Some(file) =>
        resolvePath(~env=fileEnv(file), ~path=fullPath, ~getModule)
      }
    }
  };
};

let resolveFromStamps = (~env, ~path, ~getModule, ~pos) => {
  switch (path) {
  | Tip(name) => Some((env, name))
  | Nested(name, inner) =>
    /* Log.log("Finding from stamps " ++ name); */
    switch (findInScope(pos, name, env.file.stamps.modules)) {
    | None => None
    | Some(declared) =>
      /* Log.log("found it"); */
      switch (findInModule(~env, declared.item, inner)) {
      | None => None
      | Some(res) =>
        switch (res) {
        | `Local(env, name) => Some((env, name))
        | `Global(moduleName, fullPath) =>
          switch (getModule(moduleName)) {
          | None => None
          | Some(file) =>
            resolvePath(~env=fileEnv(file), ~path=fullPath, ~getModule)
          }
        }
      }
    }
  };
};

open Infix;

let fromCompilerPath = (~env, path) => {
  switch (makePath(path)) {
  | `Stamp(stamp) => `Stamp(stamp)
  | `Path(0, moduleName, path) => `Global((moduleName, path))
  | `GlobalMod(name) => `GlobalMod(name)
  | `Path(stamp, _moduleName, path) =>
    let res =
      switch (Hashtbl.find_opt(env.file.stamps.modules, stamp)) {
      | None => None
      | Some({item: kind}) => findInModule(~env, kind, path)
      };
    switch (res) {
    | None => `Not_found
    | Some(`Local(env, name)) => `Exported((env, name))
    | Some(`Global(moduleName, fullPath)) => `Global((moduleName, fullPath))
    };
  };
};

let resolveModuleFromCompilerPath = (~env, ~getModule, path) => {
  switch (fromCompilerPath(~env, path)) {
  | `Global(moduleName, path) =>
    switch (getModule(moduleName)) {
    | None => None
    | Some(file) =>
      let env = fileEnv(file);
      switch (resolvePath(~env, ~getModule, ~path)) {
      | None => None
      | Some((env, name)) =>
        switch (Hashtbl.find_opt(env.exported.modules, name)) {
        | None => None
        | Some(stamp) =>
          switch (Hashtbl.find_opt(env.file.stamps.modules, stamp)) {
          | None => None
          | Some(declared) => Some((env, Some(declared)))
          }
        }
      };
    }
  | `Stamp(stamp) =>
    switch (Hashtbl.find_opt(env.file.stamps.modules, stamp)) {
    | None => None
    | Some(declared) => Some((env, Some(declared)))
    }
  | `GlobalMod(moduleName) =>
    switch (getModule(moduleName)) {
    | None => None
    | Some(file) =>
      let env = fileEnv(file);
      Some((env, None));
    }
  | `Not_found => None
  | `Exported(env, name) =>
    switch (Hashtbl.find_opt(env.exported.modules, name)) {
    | None => None
    | Some(stamp) =>
      switch (Hashtbl.find_opt(env.file.stamps.modules, stamp)) {
      | None => None
      | Some(declared) => Some((env, Some(declared)))
      }
    }
  };
};

let resolveFromCompilerPath = (~env, ~getModule, path) => {
  switch (fromCompilerPath(~env, path)) {
  | `Global(moduleName, path) =>
    let res =
      switch (getModule(moduleName)) {
      | None => None
      | Some(file) =>
        let env = fileEnv(file);
        resolvePath(~env, ~getModule, ~path);
      };
    switch (res) {
    | None => `Not_found
    | Some((env, name)) => `Exported((env, name))
    };

  | `Stamp(stamp) => `Stamp(stamp)
  | `GlobalMod(_) => `Not_found
  | `Not_found => `Not_found
  | `Exported(env, name) => `Exported((env, name))
  };
};

let declaredForExportedTip = (~stamps: stamps, ~exported: exported, name, tip) =>
  switch (tip) {
  | Value =>
    Hashtbl.find_opt(exported.values, name)
    |?> (
      stamp =>
        Hashtbl.find_opt(stamps.values, stamp) |?>> (x => {...x, item: ()})
    )
  | Field(_)
  | Constructor(_)
  | Type =>
    Hashtbl.find_opt(exported.types, name)
    |?> (
      stamp =>
        Hashtbl.find_opt(stamps.types, stamp) |?>> (x => {...x, item: ()})
    )
  | Module =>
    Hashtbl.find_opt(exported.modules, name)
    |?> (
      stamp =>
        Hashtbl.find_opt(stamps.modules, stamp) |?>> (x => {...x, item: ()})
    )
  };

let declaredForTip = (~stamps, stamp, tip) =>
  switch (tip) {
  | Value =>
    Hashtbl.find_opt(stamps.values, stamp) |?>> (x => {...x, item: ()})
  | Field(_)
  | Constructor(_)
  | Type =>
    Hashtbl.find_opt(stamps.types, stamp) |?>> (x => {...x, item: ()})
  | Module =>
    Hashtbl.find_opt(stamps.modules, stamp) |?>> (x => {...x, item: ()})
  };

let getField = (file, stamp, name) => {
  switch (Hashtbl.find_opt(file.stamps.types, stamp)) {
  | None => None
  | Some({item: {kind}}) =>
    switch (kind) {
    | Record(fields) => fields |> List.find_opt(f => f.fname.txt == name)
    | _ => None
    }
  };
};

let getConstructor = (file, stamp, name) => {
  switch (Hashtbl.find_opt(file.stamps.types, stamp)) {
  | None => None
  | Some({item: {kind}}) =>
    switch (kind) {
    | Variant(constructors) =>
      switch (constructors |> List.find_opt(const => const.cname.txt == name)) {
      | None => None
      | Some(const) => Some(const)
      }
    | _ => None
    }
  };
};

let exportedForTip = (~env, name, tip) =>
  switch (tip) {
  | Value => Hashtbl.find_opt(env.exported.values, name)
  | Field(_)
  | Constructor(_)
  | Type => Hashtbl.find_opt(env.exported.types, name)
  | Module => Hashtbl.find_opt(env.exported.modules, name)
  };

let rec getSourceUri = (~env, ~getModule, path) =>
  switch (path) {
  | File(uri, _moduleName) => uri
  | NotVisible => env.file.uri
  | IncludedModule(path, inner) =>
    Log.log("INCLUDED MODULE");
    switch (resolveModuleFromCompilerPath(~env, ~getModule, path)) {
    | None =>
      Log.log("NOT FOUND");
      getSourceUri(~env, ~getModule, inner);
    | Some((env, _declared)) => env.file.uri
    };
  | ExportedModule(_, inner) => getSourceUri(~env, ~getModule, inner)
  };

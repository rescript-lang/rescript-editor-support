open Comment

let withStyle style contents =
  match style with
  | `Bold -> Omd.Bold contents
  | `Italic -> Omd.Emph contents
  | `Emphasis -> Omd.Emph contents
  | `Superscript -> Omd.Raw "Superscript"
  | `Subscript -> Omd.Raw "Subscript"

let stripLoc fn item = fn item.Location_.value

let whiteLeft text =
  let ln = String.length text in
  let rec loop i =
    match i >= ln with
    | true -> i - 1
    | false -> ( match text.[i] = ' ' with true -> loop (i + 1) | false -> i )
  in
  loop 0

let sliceToEnd text num =
  let ln = String.length text in
  if ln <= num then "" else String.sub text num (ln - num)

let stripLeft text =
  let lines = Str.split (Str.regexp_string "\n") text in
  let rec loop lines =
    match lines with
    | [] -> 0
    | [one] -> whiteLeft one
    | one :: more -> min (whiteLeft one) (loop more)
  in
  let min = loop (lines |> List.filter (fun x -> String.trim x <> "")) in
  String.concat "\n" (List.map (fun line -> sliceToEnd line min) lines)

let makeHeader level content =
  match level with
  | `Title -> Omd.H1 content
  | `Section -> Omd.H2 content
  | `Subsection -> Omd.H3 content
  | `Subsubsection -> Omd.H4 content

(* [ `Module | `ModuleType | `Type
   | `Constructor | `Field | `Extension
   | `Exception | `Value | `Class | `ClassType
   | `Method | `InstanceVariable | `Label | `Page ] *)
let handleRef reference =
  match reference with
  | Paths.Reference.Root (name, _tag) -> name
  | Paths.Reference.Resolved _ -> "resolved..."
  | Paths.Reference.Dot (_, name) -> name
  | Paths.Reference.Module (_, name) -> name
  | Paths.Reference.ModuleType (_, name) -> name
  | Paths.Reference.Type (_, name) -> name
  | Paths.Reference.Constructor (_, name) -> name
  | Paths.Reference.Field (_, name) -> name
  | Paths.Reference.Extension (_, name) -> name
  | Paths.Reference.Exception (_, name) -> name
  | Paths.Reference.Value (_, name) -> name
  | Paths.Reference.Class (_, name) -> name
  | Paths.Reference.ClassType (_, name) -> name
  | Paths.Reference.Method (_, name) -> name
  | _ -> "(unhandled reference)"

let rec showPath (path : Path.module_) =
  match path with
  | Path.Resolved _resolved -> "<resolved>"
  | Path.Root name -> name
  | Path.Forward name -> name
  | Path.Dot (inner, name) -> showPath inner ^ "." ^ name
  | Path.Apply (one, two) -> showPath one ^ "(" ^ showPath two ^ ")"

let convertItem item =
  let rec convertItem item =
    match item.Location_.value with
    | `Heading (level, _label, content) ->
      makeHeader level (List.map convertLink content)
    | `Tag (`Author string) -> Omd.Text ("Author: " ^ string)
    | `Tag (`Deprecated contents) ->
      Omd.Paragraph
        (Omd.Text "Deprecated: " :: List.map (stripLoc convertNestable) contents)
    | `Tag (`Param (name, contents)) ->
      Omd.Paragraph
        ( Omd.Text ("Param: " ^ name)
        :: List.map (stripLoc convertNestable) contents )
    | `Tag (`Raise (name, contents)) ->
      Omd.Paragraph
        ( Omd.Text ("Raises: " ^ name)
        :: List.map (stripLoc convertNestable) contents )
    | `Tag (`Before (version, contents)) ->
      Omd.Paragraph
        ( Omd.Text ("Before: " ^ version)
        :: List.map (stripLoc convertNestable) contents )
    | `Tag (`Return contents) ->
      Omd.Paragraph
        (Omd.Text "Returns: " :: List.map (stripLoc convertNestable) contents)
    | `Tag (`See (_, link, contents)) ->
      Omd.Paragraph
        [
          Omd.Text "See: ";
          Omd.Url (link, List.map (stripLoc convertNestable) contents, "");
        ]
    | `Tag (`Since versionString) -> Omd.Text ("Since: " ^ versionString)
    | `Tag (`Version versionString) -> Omd.Text ("Version: " ^ versionString)
    | `Tag `Open -> Omd.Text "Open"
    | `Tag `Closed -> Omd.Text "Closed"
    | `Tag `Inline -> Omd.Text "Inline"
    | `Tag (`Canonical (path, _reference)) ->
      (* output_string(stderr, "Warning: Unhandled tag 'Canonical' in ocamldoc (please tell the reason-language-server maintainers)\n"); *)
      Omd.Text (showPath path) (* ++ ", " ++ handleRef(reference)) *)
    | `Tag _ ->
      output_string stderr
        "Warning: Unhandled tag in ocamldoc (please tell the \
         reason-language-server maintainers)\n";
      Omd.Text "Unhandled tag"
    | #nestable_block_element as item -> convertNestable item
  and convertNestable item =
    match item with
    | `Example (lang, contents) ->
      let newLang =
        match String.trim lang = "" with
        | true -> "ml"
        | false ->
          let parts = Str.split (Str.regexp_string ";") (String.trim lang) in
          if
            List.mem "ml" parts || List.mem "ocaml" parts || List.mem "re" parts
            || List.mem "reason" parts
          then lang
          else lang ^ ";ml"
      in
      Omd.Code_block (newLang, stripLeft contents)
    | `Doc contents -> Omd.Paragraph [Omd.Text ("@doc " ^ contents)]
    | `Paragraph inline -> Omd.Paragraph (List.map convertInline inline)
    | `Code_block text -> Omd.Code_block ("ml", stripLeft text)
    | `Verbatim text -> Omd.Raw text (* TODO *)
    | `Modules _ ->
      Log.log "Unhandled modules";
      Omd.Raw "!!!! Modules please"
    | `List (`Ordered, children) ->
      Omd.Ol (List.map (List.map (stripLoc convertNestable)) children)
    | `List (`Unordered, children) ->
      Omd.Ul (List.map (List.map (stripLoc convertNestable)) children)
  and convertInline item =
    match item.Location_.value with
    | `Link (href, content) -> Omd.Url (href, List.map convertLink content, "")
    | `Styled (style, contents) ->
      withStyle style (List.map convertInline contents)
    | `Reference (someref, _link) ->
      let text = handleRef someref in
      Omd.Text text
    (* Omd.Url("#TODO-ref", [Omd.Text("REFERENCE"), ...List.map(convertLink, link)], "") *)
    | #leaf_inline_element as rest -> convertLeaf rest
  and convertLink item =
    match item.Location_.value with
    | `Styled (style, contents) ->
      withStyle style (List.map convertLink contents)
    | #leaf_inline_element as rest -> convertLeaf rest
  and convertLeaf (item : Comment.leaf_inline_element) =
    match item with
    | `Space -> Omd.Text " "
    | `Word text -> Omd.Text text
    | `Code_span text -> Omd.Code ("", text)
  in
  convertItem item

let convert text =
  try
    let res =
      Parser_.parse_comment ~permissive:true ~sections_allowed:`All
        ~containing_definition:
          (Paths.Identifier.Root
             ({Root.package = "hi"; file = Page "hi"; digest = "hi"}, "What"))
        ~location:Lexing.dummy_pos ~text
    in
    match res.result with
    | Error.Ok docs -> List.map convertItem docs
    | Error message ->
      [Omd.Text ("failed to parse: " ^ Error.to_string message)]
  with exn ->
    [Omd.Text ("Error (invalid syntax?) while parsing ocamldoc: " ^ Printexc.to_string exn)]
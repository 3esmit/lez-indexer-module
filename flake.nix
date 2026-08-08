{
  description = "Logos Execution Zone Indexer Module (universal core, logos-module-builder)";

  inputs = {
    logos-module-builder.url = "github:3esmit/logos-module-builder?rev=021013458d87ba871e1d80ff2e70d8dda331606d";
    logos-execution-zone.url = "github:3esmit/logos-execution-zone?rev=51cf680b7d789bddd23f1401b1cb249f855367e2";
  };

  outputs =
    inputs@{ logos-module-builder, logos-execution-zone, ... }:
    logos-module-builder.lib.mkLogosModule {
      src = ./.;
      configFile = ./metadata.json;
      flakeInputs = inputs;
      externalLibInputs = {
        # Structured form: the dep exposes its lib under packages.<system>.indexer
        # (not .default), so map the default build variant to that package.
        indexer_ffi = {
          input = logos-execution-zone;
          packages = {
            default = "indexer";
          };
        };
      };
    };
}

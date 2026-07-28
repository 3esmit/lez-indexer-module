{
  description = "Logos Execution Zone Indexer Module (universal core, logos-module-builder)";

  inputs = {
    logos-module-builder.url = "github:logos-co/logos-module-builder";
    logos-execution-zone.url = "github:3esmit/logos-execution-zone/6e0013f5b771fc1b96a65361b531f289111b8aaa";
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

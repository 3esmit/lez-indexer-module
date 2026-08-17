{
  description = "Logos Execution Zone Indexer Module (universal core, logos-module-builder)";

  inputs = {
    logos-module-builder.url = "github:3esmit/logos-module-builder?rev=324b459c3f7b59171d249f3ccbcc362403b3fcaf";
    logos-execution-zone.url = "github:3esmit/logos-execution-zone?ref=fix/v022-legacy-chain-info";
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

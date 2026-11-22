import {Composition} from "remotion";
import {XsvCppAdvantages} from "./XsvCppAdvantages";

export const RemotionRoot = () => {
  return (
    <Composition
      id="XsvCppAdvantages"
      component={XsvCppAdvantages}
      durationInFrames={720}
      fps={30}
      width={1280}
      height={720}
    />
  );
};

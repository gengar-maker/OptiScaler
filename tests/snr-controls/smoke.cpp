#include "../../OptiScaler/dlssnr/sycl/SnrControls.h"
#include <iostream>
#include <stdexcept>
void Require(bool value, const char* reason)
{
    if (!value)
        throw std::runtime_error(reason);
}
int main()
{
    try
    {
        ArcNrSyclFrame frame {};
        frame.frame_index = 42;
        frame.reset_history = 1;
        frame.reserved = 7;
        SnrControls::Set(frame, 0, 1, 0.25f, 0.75f, false, 1);
        Require(frame.normalized_style == 0 && frame.skin_structure == -1 && frame.automatic_structure == -1,
                "AutoMask off must use -1 selectors");
        Require(frame.local_structure == 0.25f && frame.frame_index == 42 && frame.reset_history == 1 &&
                    frame.reserved == 7,
                "encoding changed unrelated frame data");
        SnrControls::Set(frame, 1, 1, 0.25f, -1, true, 1);
        Require(frame.normalized_style == 1.0f / 128 && frame.skin_structure == -1 && frame.automatic_structure == 1,
                "Natural/auto encoding mismatch");
        SnrControls::Set(frame, 2, 0.6f, 0.25f, 0.75f, true, 0.8f);
        Require(frame.normalized_style == 2.0f / 128 && frame.skin_structure == 0.75f,
                "Cinematic/skin encoding mismatch");
        auto copy = frame;
        ++copy.frame_index;
        copy.reset_history = 0;
        Require(SnrControls::Equal(frame, copy), "frame index should not invalidate controls");
        float ArcNrSyclFrame::* fields[] = { &ArcNrSyclFrame::normalized_style,    &ArcNrSyclFrame::local_tone,
                                             &ArcNrSyclFrame::local_structure,     &ArcNrSyclFrame::skin_structure,
                                             &ArcNrSyclFrame::automatic_structure, &ArcNrSyclFrame::intensity };
        for (auto field : fields)
        {
            copy = frame;
            copy.*field += 0.125f;
            Require(!SnrControls::Equal(frame, copy), "changed control did not invalidate history");
        }
        SnrControls::Set(frame, 64, 1, 1, -1, false, 1);
        Require(frame.normalized_style == 0.5f, "raw style index does not follow reference scaling");
        std::cout << "PASS reference style scaling, disabled auto mask, independent skin selector, frame data "
                     "preservation and all history-invalidating controls\n";
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

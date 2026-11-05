{
    ComPtr<ID3D12GraphicsCommandList> version;
    if (SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&version)))) {
        void** versionTable=*reinterpret_cast<void***>(version.Get());
        using H9=Hook<9,decltype(&ID3D12GraphicsCommandList::Close),Implementation>;
        if(result==NO_ERROR && !KnownTarget(9,versionTable[9])) {
            H9::original=reinterpret_cast<H9::Fn>(versionTable[9]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H9::original),H9::Call);
            if(result==NO_ERROR) staged.emplace_back(9,versionTable[9]);
        }
        using H10=Hook<10,decltype(&ID3D12GraphicsCommandList::Reset),Implementation>;
        if(result==NO_ERROR && !KnownTarget(10,versionTable[10])) {
            H10::original=reinterpret_cast<H10::Fn>(versionTable[10]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H10::original),H10::Call);
            if(result==NO_ERROR) staged.emplace_back(10,versionTable[10]);
        }
        using H11=Hook<11,decltype(&ID3D12GraphicsCommandList::ClearState),Implementation>;
        if(result==NO_ERROR && !KnownTarget(11,versionTable[11])) {
            H11::original=reinterpret_cast<H11::Fn>(versionTable[11]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H11::original),H11::Call);
            if(result==NO_ERROR) staged.emplace_back(11,versionTable[11]);
        }
        using H12=Hook<12,decltype(&ID3D12GraphicsCommandList::DrawInstanced),Implementation>;
        if(result==NO_ERROR && !KnownTarget(12,versionTable[12])) {
            H12::original=reinterpret_cast<H12::Fn>(versionTable[12]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H12::original),H12::Call);
            if(result==NO_ERROR) staged.emplace_back(12,versionTable[12]);
        }
        using H13=Hook<13,decltype(&ID3D12GraphicsCommandList::DrawIndexedInstanced),Implementation>;
        if(result==NO_ERROR && !KnownTarget(13,versionTable[13])) {
            H13::original=reinterpret_cast<H13::Fn>(versionTable[13]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H13::original),H13::Call);
            if(result==NO_ERROR) staged.emplace_back(13,versionTable[13]);
        }
        using H14=Hook<14,decltype(&ID3D12GraphicsCommandList::Dispatch),Implementation>;
        if(result==NO_ERROR && !KnownTarget(14,versionTable[14])) {
            H14::original=reinterpret_cast<H14::Fn>(versionTable[14]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H14::original),H14::Call);
            if(result==NO_ERROR) staged.emplace_back(14,versionTable[14]);
        }
        using H15=Hook<15,decltype(&ID3D12GraphicsCommandList::CopyBufferRegion),Implementation>;
        if(result==NO_ERROR && !KnownTarget(15,versionTable[15])) {
            H15::original=reinterpret_cast<H15::Fn>(versionTable[15]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H15::original),H15::Call);
            if(result==NO_ERROR) staged.emplace_back(15,versionTable[15]);
        }
        using H16=Hook<16,decltype(&ID3D12GraphicsCommandList::CopyTextureRegion),Implementation>;
        if(result==NO_ERROR && !KnownTarget(16,versionTable[16])) {
            H16::original=reinterpret_cast<H16::Fn>(versionTable[16]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H16::original),H16::Call);
            if(result==NO_ERROR) staged.emplace_back(16,versionTable[16]);
        }
        using H17=Hook<17,decltype(&ID3D12GraphicsCommandList::CopyResource),Implementation>;
        if(result==NO_ERROR && !KnownTarget(17,versionTable[17])) {
            H17::original=reinterpret_cast<H17::Fn>(versionTable[17]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H17::original),H17::Call);
            if(result==NO_ERROR) staged.emplace_back(17,versionTable[17]);
        }
        using H18=Hook<18,decltype(&ID3D12GraphicsCommandList::CopyTiles),Implementation>;
        if(result==NO_ERROR && !KnownTarget(18,versionTable[18])) {
            H18::original=reinterpret_cast<H18::Fn>(versionTable[18]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H18::original),H18::Call);
            if(result==NO_ERROR) staged.emplace_back(18,versionTable[18]);
        }
        using H19=Hook<19,decltype(&ID3D12GraphicsCommandList::ResolveSubresource),Implementation>;
        if(result==NO_ERROR && !KnownTarget(19,versionTable[19])) {
            H19::original=reinterpret_cast<H19::Fn>(versionTable[19]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H19::original),H19::Call);
            if(result==NO_ERROR) staged.emplace_back(19,versionTable[19]);
        }
        using H20=Hook<20,decltype(&ID3D12GraphicsCommandList::IASetPrimitiveTopology),Implementation>;
        if(result==NO_ERROR && !KnownTarget(20,versionTable[20])) {
            H20::original=reinterpret_cast<H20::Fn>(versionTable[20]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H20::original),H20::Call);
            if(result==NO_ERROR) staged.emplace_back(20,versionTable[20]);
        }
        using H21=Hook<21,decltype(&ID3D12GraphicsCommandList::RSSetViewports),Implementation>;
        if(result==NO_ERROR && !KnownTarget(21,versionTable[21])) {
            H21::original=reinterpret_cast<H21::Fn>(versionTable[21]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H21::original),H21::Call);
            if(result==NO_ERROR) staged.emplace_back(21,versionTable[21]);
        }
        using H22=Hook<22,decltype(&ID3D12GraphicsCommandList::RSSetScissorRects),Implementation>;
        if(result==NO_ERROR && !KnownTarget(22,versionTable[22])) {
            H22::original=reinterpret_cast<H22::Fn>(versionTable[22]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H22::original),H22::Call);
            if(result==NO_ERROR) staged.emplace_back(22,versionTable[22]);
        }
        using H23=Hook<23,decltype(&ID3D12GraphicsCommandList::OMSetBlendFactor),Implementation>;
        if(result==NO_ERROR && !KnownTarget(23,versionTable[23])) {
            H23::original=reinterpret_cast<H23::Fn>(versionTable[23]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H23::original),H23::Call);
            if(result==NO_ERROR) staged.emplace_back(23,versionTable[23]);
        }
        using H24=Hook<24,decltype(&ID3D12GraphicsCommandList::OMSetStencilRef),Implementation>;
        if(result==NO_ERROR && !KnownTarget(24,versionTable[24])) {
            H24::original=reinterpret_cast<H24::Fn>(versionTable[24]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H24::original),H24::Call);
            if(result==NO_ERROR) staged.emplace_back(24,versionTable[24]);
        }
        using H25=Hook<25,decltype(&ID3D12GraphicsCommandList::SetPipelineState),Implementation>;
        if(result==NO_ERROR && !KnownTarget(25,versionTable[25])) {
            H25::original=reinterpret_cast<H25::Fn>(versionTable[25]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H25::original),H25::Call);
            if(result==NO_ERROR) staged.emplace_back(25,versionTable[25]);
        }
        using H26=Hook<26,decltype(&ID3D12GraphicsCommandList::ResourceBarrier),Implementation>;
        if(result==NO_ERROR && !KnownTarget(26,versionTable[26])) {
            H26::original=reinterpret_cast<H26::Fn>(versionTable[26]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H26::original),H26::Call);
            if(result==NO_ERROR) staged.emplace_back(26,versionTable[26]);
        }
        using H27=Hook<27,decltype(&ID3D12GraphicsCommandList::ExecuteBundle),Implementation>;
        if(result==NO_ERROR && !KnownTarget(27,versionTable[27])) {
            H27::original=reinterpret_cast<H27::Fn>(versionTable[27]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H27::original),H27::Call);
            if(result==NO_ERROR) staged.emplace_back(27,versionTable[27]);
        }
        using H28=Hook<28,decltype(&ID3D12GraphicsCommandList::SetDescriptorHeaps),Implementation>;
        if(result==NO_ERROR && !KnownTarget(28,versionTable[28])) {
            H28::original=reinterpret_cast<H28::Fn>(versionTable[28]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H28::original),H28::Call);
            if(result==NO_ERROR) staged.emplace_back(28,versionTable[28]);
        }
        using H29=Hook<29,decltype(&ID3D12GraphicsCommandList::SetComputeRootSignature),Implementation>;
        if(result==NO_ERROR && !KnownTarget(29,versionTable[29])) {
            H29::original=reinterpret_cast<H29::Fn>(versionTable[29]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H29::original),H29::Call);
            if(result==NO_ERROR) staged.emplace_back(29,versionTable[29]);
        }
        using H30=Hook<30,decltype(&ID3D12GraphicsCommandList::SetGraphicsRootSignature),Implementation>;
        if(result==NO_ERROR && !KnownTarget(30,versionTable[30])) {
            H30::original=reinterpret_cast<H30::Fn>(versionTable[30]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H30::original),H30::Call);
            if(result==NO_ERROR) staged.emplace_back(30,versionTable[30]);
        }
        using H31=Hook<31,decltype(&ID3D12GraphicsCommandList::SetComputeRootDescriptorTable),Implementation>;
        if(result==NO_ERROR && !KnownTarget(31,versionTable[31])) {
            H31::original=reinterpret_cast<H31::Fn>(versionTable[31]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H31::original),H31::Call);
            if(result==NO_ERROR) staged.emplace_back(31,versionTable[31]);
        }
        using H32=Hook<32,decltype(&ID3D12GraphicsCommandList::SetGraphicsRootDescriptorTable),Implementation>;
        if(result==NO_ERROR && !KnownTarget(32,versionTable[32])) {
            H32::original=reinterpret_cast<H32::Fn>(versionTable[32]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H32::original),H32::Call);
            if(result==NO_ERROR) staged.emplace_back(32,versionTable[32]);
        }
        using H33=Hook<33,decltype(&ID3D12GraphicsCommandList::SetComputeRoot32BitConstant),Implementation>;
        if(result==NO_ERROR && !KnownTarget(33,versionTable[33])) {
            H33::original=reinterpret_cast<H33::Fn>(versionTable[33]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H33::original),H33::Call);
            if(result==NO_ERROR) staged.emplace_back(33,versionTable[33]);
        }
        using H34=Hook<34,decltype(&ID3D12GraphicsCommandList::SetGraphicsRoot32BitConstant),Implementation>;
        if(result==NO_ERROR && !KnownTarget(34,versionTable[34])) {
            H34::original=reinterpret_cast<H34::Fn>(versionTable[34]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H34::original),H34::Call);
            if(result==NO_ERROR) staged.emplace_back(34,versionTable[34]);
        }
        using H35=Hook<35,decltype(&ID3D12GraphicsCommandList::SetComputeRoot32BitConstants),Implementation>;
        if(result==NO_ERROR && !KnownTarget(35,versionTable[35])) {
            H35::original=reinterpret_cast<H35::Fn>(versionTable[35]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H35::original),H35::Call);
            if(result==NO_ERROR) staged.emplace_back(35,versionTable[35]);
        }
        using H36=Hook<36,decltype(&ID3D12GraphicsCommandList::SetGraphicsRoot32BitConstants),Implementation>;
        if(result==NO_ERROR && !KnownTarget(36,versionTable[36])) {
            H36::original=reinterpret_cast<H36::Fn>(versionTable[36]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H36::original),H36::Call);
            if(result==NO_ERROR) staged.emplace_back(36,versionTable[36]);
        }
        using H37=Hook<37,decltype(&ID3D12GraphicsCommandList::SetComputeRootConstantBufferView),Implementation>;
        if(result==NO_ERROR && !KnownTarget(37,versionTable[37])) {
            H37::original=reinterpret_cast<H37::Fn>(versionTable[37]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H37::original),H37::Call);
            if(result==NO_ERROR) staged.emplace_back(37,versionTable[37]);
        }
        using H38=Hook<38,decltype(&ID3D12GraphicsCommandList::SetGraphicsRootConstantBufferView),Implementation>;
        if(result==NO_ERROR && !KnownTarget(38,versionTable[38])) {
            H38::original=reinterpret_cast<H38::Fn>(versionTable[38]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H38::original),H38::Call);
            if(result==NO_ERROR) staged.emplace_back(38,versionTable[38]);
        }
        using H39=Hook<39,decltype(&ID3D12GraphicsCommandList::SetComputeRootShaderResourceView),Implementation>;
        if(result==NO_ERROR && !KnownTarget(39,versionTable[39])) {
            H39::original=reinterpret_cast<H39::Fn>(versionTable[39]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H39::original),H39::Call);
            if(result==NO_ERROR) staged.emplace_back(39,versionTable[39]);
        }
        using H40=Hook<40,decltype(&ID3D12GraphicsCommandList::SetGraphicsRootShaderResourceView),Implementation>;
        if(result==NO_ERROR && !KnownTarget(40,versionTable[40])) {
            H40::original=reinterpret_cast<H40::Fn>(versionTable[40]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H40::original),H40::Call);
            if(result==NO_ERROR) staged.emplace_back(40,versionTable[40]);
        }
        using H41=Hook<41,decltype(&ID3D12GraphicsCommandList::SetComputeRootUnorderedAccessView),Implementation>;
        if(result==NO_ERROR && !KnownTarget(41,versionTable[41])) {
            H41::original=reinterpret_cast<H41::Fn>(versionTable[41]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H41::original),H41::Call);
            if(result==NO_ERROR) staged.emplace_back(41,versionTable[41]);
        }
        using H42=Hook<42,decltype(&ID3D12GraphicsCommandList::SetGraphicsRootUnorderedAccessView),Implementation>;
        if(result==NO_ERROR && !KnownTarget(42,versionTable[42])) {
            H42::original=reinterpret_cast<H42::Fn>(versionTable[42]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H42::original),H42::Call);
            if(result==NO_ERROR) staged.emplace_back(42,versionTable[42]);
        }
        using H43=Hook<43,decltype(&ID3D12GraphicsCommandList::IASetIndexBuffer),Implementation>;
        if(result==NO_ERROR && !KnownTarget(43,versionTable[43])) {
            H43::original=reinterpret_cast<H43::Fn>(versionTable[43]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H43::original),H43::Call);
            if(result==NO_ERROR) staged.emplace_back(43,versionTable[43]);
        }
        using H44=Hook<44,decltype(&ID3D12GraphicsCommandList::IASetVertexBuffers),Implementation>;
        if(result==NO_ERROR && !KnownTarget(44,versionTable[44])) {
            H44::original=reinterpret_cast<H44::Fn>(versionTable[44]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H44::original),H44::Call);
            if(result==NO_ERROR) staged.emplace_back(44,versionTable[44]);
        }
        using H45=Hook<45,decltype(&ID3D12GraphicsCommandList::SOSetTargets),Implementation>;
        if(result==NO_ERROR && !KnownTarget(45,versionTable[45])) {
            H45::original=reinterpret_cast<H45::Fn>(versionTable[45]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H45::original),H45::Call);
            if(result==NO_ERROR) staged.emplace_back(45,versionTable[45]);
        }
        using H46=Hook<46,decltype(&ID3D12GraphicsCommandList::OMSetRenderTargets),Implementation>;
        if(result==NO_ERROR && !KnownTarget(46,versionTable[46])) {
            H46::original=reinterpret_cast<H46::Fn>(versionTable[46]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H46::original),H46::Call);
            if(result==NO_ERROR) staged.emplace_back(46,versionTable[46]);
        }
        using H47=Hook<47,decltype(&ID3D12GraphicsCommandList::ClearDepthStencilView),Implementation>;
        if(result==NO_ERROR && !KnownTarget(47,versionTable[47])) {
            H47::original=reinterpret_cast<H47::Fn>(versionTable[47]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H47::original),H47::Call);
            if(result==NO_ERROR) staged.emplace_back(47,versionTable[47]);
        }
        using H48=Hook<48,decltype(&ID3D12GraphicsCommandList::ClearRenderTargetView),Implementation>;
        if(result==NO_ERROR && !KnownTarget(48,versionTable[48])) {
            H48::original=reinterpret_cast<H48::Fn>(versionTable[48]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H48::original),H48::Call);
            if(result==NO_ERROR) staged.emplace_back(48,versionTable[48]);
        }
        using H49=Hook<49,decltype(&ID3D12GraphicsCommandList::ClearUnorderedAccessViewUint),Implementation>;
        if(result==NO_ERROR && !KnownTarget(49,versionTable[49])) {
            H49::original=reinterpret_cast<H49::Fn>(versionTable[49]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H49::original),H49::Call);
            if(result==NO_ERROR) staged.emplace_back(49,versionTable[49]);
        }
        using H50=Hook<50,decltype(&ID3D12GraphicsCommandList::ClearUnorderedAccessViewFloat),Implementation>;
        if(result==NO_ERROR && !KnownTarget(50,versionTable[50])) {
            H50::original=reinterpret_cast<H50::Fn>(versionTable[50]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H50::original),H50::Call);
            if(result==NO_ERROR) staged.emplace_back(50,versionTable[50]);
        }
        using H51=Hook<51,decltype(&ID3D12GraphicsCommandList::DiscardResource),Implementation>;
        if(result==NO_ERROR && !KnownTarget(51,versionTable[51])) {
            H51::original=reinterpret_cast<H51::Fn>(versionTable[51]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H51::original),H51::Call);
            if(result==NO_ERROR) staged.emplace_back(51,versionTable[51]);
        }
        using H52=Hook<52,decltype(&ID3D12GraphicsCommandList::BeginQuery),Implementation>;
        if(result==NO_ERROR && !KnownTarget(52,versionTable[52])) {
            H52::original=reinterpret_cast<H52::Fn>(versionTable[52]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H52::original),H52::Call);
            if(result==NO_ERROR) staged.emplace_back(52,versionTable[52]);
        }
        using H53=Hook<53,decltype(&ID3D12GraphicsCommandList::EndQuery),Implementation>;
        if(result==NO_ERROR && !KnownTarget(53,versionTable[53])) {
            H53::original=reinterpret_cast<H53::Fn>(versionTable[53]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H53::original),H53::Call);
            if(result==NO_ERROR) staged.emplace_back(53,versionTable[53]);
        }
        using H54=Hook<54,decltype(&ID3D12GraphicsCommandList::ResolveQueryData),Implementation>;
        if(result==NO_ERROR && !KnownTarget(54,versionTable[54])) {
            H54::original=reinterpret_cast<H54::Fn>(versionTable[54]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H54::original),H54::Call);
            if(result==NO_ERROR) staged.emplace_back(54,versionTable[54]);
        }
        using H55=Hook<55,decltype(&ID3D12GraphicsCommandList::SetPredication),Implementation>;
        if(result==NO_ERROR && !KnownTarget(55,versionTable[55])) {
            H55::original=reinterpret_cast<H55::Fn>(versionTable[55]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H55::original),H55::Call);
            if(result==NO_ERROR) staged.emplace_back(55,versionTable[55]);
        }
        using H56=Hook<56,decltype(&ID3D12GraphicsCommandList::SetMarker),Implementation>;
        if(result==NO_ERROR && !KnownTarget(56,versionTable[56])) {
            H56::original=reinterpret_cast<H56::Fn>(versionTable[56]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H56::original),H56::Call);
            if(result==NO_ERROR) staged.emplace_back(56,versionTable[56]);
        }
        using H57=Hook<57,decltype(&ID3D12GraphicsCommandList::BeginEvent),Implementation>;
        if(result==NO_ERROR && !KnownTarget(57,versionTable[57])) {
            H57::original=reinterpret_cast<H57::Fn>(versionTable[57]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H57::original),H57::Call);
            if(result==NO_ERROR) staged.emplace_back(57,versionTable[57]);
        }
        using H58=Hook<58,decltype(&ID3D12GraphicsCommandList::EndEvent),Implementation>;
        if(result==NO_ERROR && !KnownTarget(58,versionTable[58])) {
            H58::original=reinterpret_cast<H58::Fn>(versionTable[58]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H58::original),H58::Call);
            if(result==NO_ERROR) staged.emplace_back(58,versionTable[58]);
        }
        using H59=Hook<59,decltype(&ID3D12GraphicsCommandList::ExecuteIndirect),Implementation>;
        if(result==NO_ERROR && !KnownTarget(59,versionTable[59])) {
            H59::original=reinterpret_cast<H59::Fn>(versionTable[59]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H59::original),H59::Call);
            if(result==NO_ERROR) staged.emplace_back(59,versionTable[59]);
        }
    }
}
{
    ComPtr<ID3D12GraphicsCommandList1> version;
    if (SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&version)))) {
        void** versionTable=*reinterpret_cast<void***>(version.Get());
        using H60=Hook<60,decltype(&ID3D12GraphicsCommandList1::AtomicCopyBufferUINT),Implementation>;
        if(result==NO_ERROR && !KnownTarget(60,versionTable[60])) {
            H60::original=reinterpret_cast<H60::Fn>(versionTable[60]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H60::original),H60::Call);
            if(result==NO_ERROR) staged.emplace_back(60,versionTable[60]);
        }
        using H61=Hook<61,decltype(&ID3D12GraphicsCommandList1::AtomicCopyBufferUINT64),Implementation>;
        if(result==NO_ERROR && !KnownTarget(61,versionTable[61])) {
            H61::original=reinterpret_cast<H61::Fn>(versionTable[61]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H61::original),H61::Call);
            if(result==NO_ERROR) staged.emplace_back(61,versionTable[61]);
        }
        using H62=Hook<62,decltype(&ID3D12GraphicsCommandList1::OMSetDepthBounds),Implementation>;
        if(result==NO_ERROR && !KnownTarget(62,versionTable[62])) {
            H62::original=reinterpret_cast<H62::Fn>(versionTable[62]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H62::original),H62::Call);
            if(result==NO_ERROR) staged.emplace_back(62,versionTable[62]);
        }
        using H63=Hook<63,decltype(&ID3D12GraphicsCommandList1::SetSamplePositions),Implementation>;
        if(result==NO_ERROR && !KnownTarget(63,versionTable[63])) {
            H63::original=reinterpret_cast<H63::Fn>(versionTable[63]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H63::original),H63::Call);
            if(result==NO_ERROR) staged.emplace_back(63,versionTable[63]);
        }
        using H64=Hook<64,decltype(&ID3D12GraphicsCommandList1::ResolveSubresourceRegion),Implementation>;
        if(result==NO_ERROR && !KnownTarget(64,versionTable[64])) {
            H64::original=reinterpret_cast<H64::Fn>(versionTable[64]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H64::original),H64::Call);
            if(result==NO_ERROR) staged.emplace_back(64,versionTable[64]);
        }
        using H65=Hook<65,decltype(&ID3D12GraphicsCommandList1::SetViewInstanceMask),Implementation>;
        if(result==NO_ERROR && !KnownTarget(65,versionTable[65])) {
            H65::original=reinterpret_cast<H65::Fn>(versionTable[65]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H65::original),H65::Call);
            if(result==NO_ERROR) staged.emplace_back(65,versionTable[65]);
        }
    }
}
{
    ComPtr<ID3D12GraphicsCommandList2> version;
    if (SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&version)))) {
        void** versionTable=*reinterpret_cast<void***>(version.Get());
        using H66=Hook<66,decltype(&ID3D12GraphicsCommandList2::WriteBufferImmediate),Implementation>;
        if(result==NO_ERROR && !KnownTarget(66,versionTable[66])) {
            H66::original=reinterpret_cast<H66::Fn>(versionTable[66]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H66::original),H66::Call);
            if(result==NO_ERROR) staged.emplace_back(66,versionTable[66]);
        }
    }
}
{
    ComPtr<ID3D12GraphicsCommandList3> version;
    if (SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&version)))) {
        void** versionTable=*reinterpret_cast<void***>(version.Get());
        using H67=Hook<67,decltype(&ID3D12GraphicsCommandList3::SetProtectedResourceSession),Implementation>;
        if(result==NO_ERROR && !KnownTarget(67,versionTable[67])) {
            H67::original=reinterpret_cast<H67::Fn>(versionTable[67]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H67::original),H67::Call);
            if(result==NO_ERROR) staged.emplace_back(67,versionTable[67]);
        }
    }
}
{
    ComPtr<ID3D12GraphicsCommandList4> version;
    if (SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&version)))) {
        void** versionTable=*reinterpret_cast<void***>(version.Get());
        using H68=Hook<68,decltype(&ID3D12GraphicsCommandList4::BeginRenderPass),Implementation>;
        if(result==NO_ERROR && !KnownTarget(68,versionTable[68])) {
            H68::original=reinterpret_cast<H68::Fn>(versionTable[68]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H68::original),H68::Call);
            if(result==NO_ERROR) staged.emplace_back(68,versionTable[68]);
        }
        using H69=Hook<69,decltype(&ID3D12GraphicsCommandList4::EndRenderPass),Implementation>;
        if(result==NO_ERROR && !KnownTarget(69,versionTable[69])) {
            H69::original=reinterpret_cast<H69::Fn>(versionTable[69]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H69::original),H69::Call);
            if(result==NO_ERROR) staged.emplace_back(69,versionTable[69]);
        }
        using H70=Hook<70,decltype(&ID3D12GraphicsCommandList4::InitializeMetaCommand),Implementation>;
        if(result==NO_ERROR && !KnownTarget(70,versionTable[70])) {
            H70::original=reinterpret_cast<H70::Fn>(versionTable[70]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H70::original),H70::Call);
            if(result==NO_ERROR) staged.emplace_back(70,versionTable[70]);
        }
        using H71=Hook<71,decltype(&ID3D12GraphicsCommandList4::ExecuteMetaCommand),Implementation>;
        if(result==NO_ERROR && !KnownTarget(71,versionTable[71])) {
            H71::original=reinterpret_cast<H71::Fn>(versionTable[71]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H71::original),H71::Call);
            if(result==NO_ERROR) staged.emplace_back(71,versionTable[71]);
        }
        using H72=Hook<72,decltype(&ID3D12GraphicsCommandList4::BuildRaytracingAccelerationStructure),Implementation>;
        if(result==NO_ERROR && !KnownTarget(72,versionTable[72])) {
            H72::original=reinterpret_cast<H72::Fn>(versionTable[72]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H72::original),H72::Call);
            if(result==NO_ERROR) staged.emplace_back(72,versionTable[72]);
        }
        using H73=Hook<73,decltype(&ID3D12GraphicsCommandList4::EmitRaytracingAccelerationStructurePostbuildInfo),Implementation>;
        if(result==NO_ERROR && !KnownTarget(73,versionTable[73])) {
            H73::original=reinterpret_cast<H73::Fn>(versionTable[73]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H73::original),H73::Call);
            if(result==NO_ERROR) staged.emplace_back(73,versionTable[73]);
        }
        using H74=Hook<74,decltype(&ID3D12GraphicsCommandList4::CopyRaytracingAccelerationStructure),Implementation>;
        if(result==NO_ERROR && !KnownTarget(74,versionTable[74])) {
            H74::original=reinterpret_cast<H74::Fn>(versionTable[74]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H74::original),H74::Call);
            if(result==NO_ERROR) staged.emplace_back(74,versionTable[74]);
        }
        using H75=Hook<75,decltype(&ID3D12GraphicsCommandList4::SetPipelineState1),Implementation>;
        if(result==NO_ERROR && !KnownTarget(75,versionTable[75])) {
            H75::original=reinterpret_cast<H75::Fn>(versionTable[75]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H75::original),H75::Call);
            if(result==NO_ERROR) staged.emplace_back(75,versionTable[75]);
        }
        using H76=Hook<76,decltype(&ID3D12GraphicsCommandList4::DispatchRays),Implementation>;
        if(result==NO_ERROR && !KnownTarget(76,versionTable[76])) {
            H76::original=reinterpret_cast<H76::Fn>(versionTable[76]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H76::original),H76::Call);
            if(result==NO_ERROR) staged.emplace_back(76,versionTable[76]);
        }
    }
}
{
    ComPtr<ID3D12GraphicsCommandList5> version;
    if (SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&version)))) {
        void** versionTable=*reinterpret_cast<void***>(version.Get());
        using H77=Hook<77,decltype(&ID3D12GraphicsCommandList5::RSSetShadingRate),Implementation>;
        if(result==NO_ERROR && !KnownTarget(77,versionTable[77])) {
            H77::original=reinterpret_cast<H77::Fn>(versionTable[77]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H77::original),H77::Call);
            if(result==NO_ERROR) staged.emplace_back(77,versionTable[77]);
        }
        using H78=Hook<78,decltype(&ID3D12GraphicsCommandList5::RSSetShadingRateImage),Implementation>;
        if(result==NO_ERROR && !KnownTarget(78,versionTable[78])) {
            H78::original=reinterpret_cast<H78::Fn>(versionTable[78]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H78::original),H78::Call);
            if(result==NO_ERROR) staged.emplace_back(78,versionTable[78]);
        }
    }
}
{
    ComPtr<ID3D12GraphicsCommandList6> version;
    if (SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&version)))) {
        void** versionTable=*reinterpret_cast<void***>(version.Get());
        using H79=Hook<79,decltype(&ID3D12GraphicsCommandList6::DispatchMesh),Implementation>;
        if(result==NO_ERROR && !KnownTarget(79,versionTable[79])) {
            H79::original=reinterpret_cast<H79::Fn>(versionTable[79]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H79::original),H79::Call);
            if(result==NO_ERROR) staged.emplace_back(79,versionTable[79]);
        }
    }
}
{
    ComPtr<ID3D12GraphicsCommandList7> version;
    if (SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&version)))) {
        void** versionTable=*reinterpret_cast<void***>(version.Get());
        using H80=Hook<80,decltype(&ID3D12GraphicsCommandList7::Barrier),Implementation>;
        if(result==NO_ERROR && !KnownTarget(80,versionTable[80])) {
            H80::original=reinterpret_cast<H80::Fn>(versionTable[80]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H80::original),H80::Call);
            if(result==NO_ERROR) staged.emplace_back(80,versionTable[80]);
        }
    }
}
{
    ComPtr<ID3D12GraphicsCommandList8> version;
    if (SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&version)))) {
        void** versionTable=*reinterpret_cast<void***>(version.Get());
        using H81=Hook<81,decltype(&ID3D12GraphicsCommandList8::OMSetFrontAndBackStencilRef),Implementation>;
        if(result==NO_ERROR && !KnownTarget(81,versionTable[81])) {
            H81::original=reinterpret_cast<H81::Fn>(versionTable[81]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H81::original),H81::Call);
            if(result==NO_ERROR) staged.emplace_back(81,versionTable[81]);
        }
    }
}
{
    ComPtr<ID3D12GraphicsCommandList9> version;
    if (SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&version)))) {
        void** versionTable=*reinterpret_cast<void***>(version.Get());
        using H82=Hook<82,decltype(&ID3D12GraphicsCommandList9::RSSetDepthBias),Implementation>;
        if(result==NO_ERROR && !KnownTarget(82,versionTable[82])) {
            H82::original=reinterpret_cast<H82::Fn>(versionTable[82]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H82::original),H82::Call);
            if(result==NO_ERROR) staged.emplace_back(82,versionTable[82]);
        }
        using H83=Hook<83,decltype(&ID3D12GraphicsCommandList9::IASetIndexBufferStripCutValue),Implementation>;
        if(result==NO_ERROR && !KnownTarget(83,versionTable[83])) {
            H83::original=reinterpret_cast<H83::Fn>(versionTable[83]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H83::original),H83::Call);
            if(result==NO_ERROR) staged.emplace_back(83,versionTable[83]);
        }
    }
}
{
    ComPtr<ID3D12GraphicsCommandList10> version;
    if (SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&version)))) {
        void** versionTable=*reinterpret_cast<void***>(version.Get());
        using H84=Hook<84,decltype(&ID3D12GraphicsCommandList10::SetProgram),Implementation>;
        if(result==NO_ERROR && !KnownTarget(84,versionTable[84])) {
            H84::original=reinterpret_cast<H84::Fn>(versionTable[84]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H84::original),H84::Call);
            if(result==NO_ERROR) staged.emplace_back(84,versionTable[84]);
        }
        using H85=Hook<85,decltype(&ID3D12GraphicsCommandList10::DispatchGraph),Implementation>;
        if(result==NO_ERROR && !KnownTarget(85,versionTable[85])) {
            H85::original=reinterpret_cast<H85::Fn>(versionTable[85]);
            result=DetourAttach(reinterpret_cast<PVOID*>(&H85::original),H85::Call);
            if(result==NO_ERROR) staged.emplace_back(85,versionTable[85]);
        }
    }
}
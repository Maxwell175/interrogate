#nullable enable

using System;
using System.Runtime.InteropServices;
using Interrogate;

namespace Advanced.Features {
  internal static partial class NativeMethods {
    [LibraryImport("advanced_features", EntryPoint = "_inCEIF3veB5")]
    internal static partial IntPtr Shape_Shape_veB5();

    [LibraryImport("advanced_features", EntryPoint = "_inCEIF3_TL_")]
    internal static partial IntPtr Shape_Shape_TL(IntPtr param0);

    [LibraryImport("advanced_features", EntryPoint = "_inCEIF3HTb5")]
    internal static partial float Shape_area_HTb5(IntPtr _this);

    [LibraryImport("advanced_features", EntryPoint = "_inCEIF3TvJn", StringMarshalling = StringMarshalling.Utf8)]
    internal static partial string Shape_type_name_TvJn(IntPtr _this);

    [LibraryImport("advanced_features", EntryPoint = "_inCEIF3Dam3")]
    internal static partial int Shape_get_id_Dam3(IntPtr _this);

    [LibraryImport("advanced_features", EntryPoint = "_inCEIF3hOSs")]
    internal static partial IntPtr Circle_Circle_hOSs(IntPtr param0);

    [LibraryImport("advanced_features", EntryPoint = "_inCEIF3X2F7")]
    internal static partial IntPtr Circle_Circle_X2F7(float radius);

    [LibraryImport("advanced_features", EntryPoint = "_inCEIF3AiYO")]
    internal static partial float Circle_get_radius_AiYO(IntPtr _this);

    [LibraryImport("advanced_features", EntryPoint = "_inCEIF3Kk_F")]
    internal static partial void Circle_set_radius_Kk_F(IntPtr _this, float r);

    [LibraryImport("advanced_features", EntryPoint = "_inCEIF3TSCe")]
    internal static partial IntPtr Serializable_Serializable_TSCe();

    [LibraryImport("advanced_features", EntryPoint = "_inCEIF31dfC")]
    internal static partial IntPtr Serializable_Serializable_1dfC(IntPtr param0);

    [LibraryImport("advanced_features", EntryPoint = "_inCEIF3ME_f", StringMarshalling = StringMarshalling.Utf8)]
    internal static partial string Serializable_serialize_ME_f(IntPtr _this);

    [LibraryImport("advanced_features", EntryPoint = "_inCEIF3H2S6")]
    internal static partial IntPtr Printable_Printable_H2S6();

    [LibraryImport("advanced_features", EntryPoint = "_inCEIF3jkyB")]
    internal static partial IntPtr Printable_Printable_jkyB(IntPtr param0);

    [LibraryImport("advanced_features", EntryPoint = "_inCEIF3md1D", StringMarshalling = StringMarshalling.Utf8)]
    internal static partial string Printable_to_string_md1D(IntPtr _this);

    [LibraryImport("advanced_features", EntryPoint = "_inCEIF3ebSd")]
    internal static partial IntPtr Document_upcast_to_Serializable_ebSd(IntPtr _this);

    [LibraryImport("advanced_features", EntryPoint = "_inCEIF3yrGB")]
    internal static partial IntPtr Serializable_downcast_to_Document_yrGB(IntPtr _this);

    [LibraryImport("advanced_features", EntryPoint = "_inCEIF3RNdf")]
    internal static partial IntPtr Document_upcast_to_Printable_RNdf(IntPtr _this);

    [LibraryImport("advanced_features", EntryPoint = "_inCEIF3NvhN")]
    internal static partial IntPtr Printable_downcast_to_Document_NvhN(IntPtr _this);

    [LibraryImport("advanced_features", EntryPoint = "_inCEIF3VzBg")]
    internal static partial IntPtr Document_Document_VzBg(IntPtr param0);

    [LibraryImport("advanced_features", EntryPoint = "_inCEIF3pA2S", StringMarshalling = StringMarshalling.Utf8)]
    internal static partial IntPtr Document_Document_pA2S(string title);

    [LibraryImport("advanced_features", EntryPoint = "_inCEIF3K0CU", StringMarshalling = StringMarshalling.Utf8)]
    internal static partial string Document_get_title_K0CU(IntPtr _this);

    [LibraryImport("advanced_features", EntryPoint = "_inCEIF3obch", StringMarshalling = StringMarshalling.Utf8)]
    internal static partial string Document_serialize_obch(IntPtr _this);

    [LibraryImport("advanced_features", EntryPoint = "_inCEIF3q8Qd", StringMarshalling = StringMarshalling.Utf8)]
    internal static partial string Document_to_string_q8Qd(IntPtr _this);

    [LibraryImport("advanced_features", EntryPoint = "_inCEIF36TOC")]
    internal static partial void Renderable_render_6TOC(IntPtr _this);

    [LibraryImport("advanced_features", EntryPoint = "_inCEIF34xva")]
    internal static partial int Renderable_vertex_count_4xva(IntPtr _this);

    [LibraryImport("advanced_features", EntryPoint = "_inCEIF3YolD")]
    internal static partial IntPtr Mesh_Mesh_YolD(int num_verts);

    [LibraryImport("advanced_features", EntryPoint = "_inCEIF3bJR5")]
    internal static partial IntPtr IntArray_IntArray_bJR5();

    [LibraryImport("advanced_features", EntryPoint = "_inCEIF3BIPk")]
    internal static partial IntPtr IntArray_IntArray_BIPk(IntPtr param0);

    [LibraryImport("advanced_features", EntryPoint = "_inCEIF3zXDN")]
    internal static partial int IntArray_get_num_elements_zXDN(IntPtr _this);

    [LibraryImport("advanced_features", EntryPoint = "_inCEIF3RD3X")]
    internal static partial int IntArray_get_element_RD3X(IntPtr _this, int n);

    [LibraryImport("advanced_features", EntryPoint = "_inCEIF3ZAs0")]
    internal static partial void IntArray_add_element_ZAs0(IntPtr _this, int val);

    [LibraryImport("advanced_features", EntryPoint = "_inCSDestr_Shape")]
    internal static partial void Destroy_Shape(IntPtr self);

    [LibraryImport("advanced_features", EntryPoint = "_inCSDestr_Circle")]
    internal static partial void Destroy_Circle(IntPtr self);

    [LibraryImport("advanced_features", EntryPoint = "_inCSDestr_Serializable")]
    internal static partial void Destroy_Serializable(IntPtr self);

    [LibraryImport("advanced_features", EntryPoint = "_inCSDestr_Printable")]
    internal static partial void Destroy_Printable(IntPtr self);

    [LibraryImport("advanced_features", EntryPoint = "_inCSDestr_Document")]
    internal static partial void Destroy_Document(IntPtr self);

    [LibraryImport("advanced_features", EntryPoint = "_inCSDestr_Renderable")]
    internal static partial void Destroy_Renderable(IntPtr self);

    [LibraryImport("advanced_features", EntryPoint = "_inCSDestr_Mesh")]
    internal static partial void Destroy_Mesh(IntPtr self);

    [LibraryImport("advanced_features", EntryPoint = "_inCSDestr_IntArray")]
    internal static partial void Destroy_IntArray(IntPtr self);

  }
}

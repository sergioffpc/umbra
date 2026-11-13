//! Math launches: `umbra-math`'s functions evaluated on the GPU by the math
//! program, for the GPU tests that check the GPU's math against the CPU's
//! (#146). With the `math-program` feature alone.

use cudarc::driver::{DevicePtr, DevicePtrMut};
use umbra_gpu_programs::{MathFunction, MathParams};

use super::Tracer;
use crate::optix::ShaderBindingTable;
use crate::records::DeviceCopy;
use crate::{Error, Geometry};

impl Tracer {
    /// Evaluates `function` on the GPU, once per record of `args`, which
    /// holds its arguments in order, the unused ones ignored: `[x, y, z]`
    /// for `fma(x, y, z)`, `[y, x, _]` for `atan2(y, x)`, `[x, _, _]` for a
    /// function of one argument.
    ///
    /// # Errors
    ///
    /// [`Error::TooMany`] beyond `u32::MAX` records, and [`Error::Cuda`] or
    /// [`Error::Optix`] when the launch fails.
    pub fn evaluate(&self, function: MathFunction, args: &[[f32; 3]]) -> Result<Vec<f32>, Error> {
        if args.is_empty() {
            return Ok(Vec::new());
        }
        let count = u32::try_from(args.len()).map_err(|_| Error::TooMany("arguments"))?;
        let arg_buffer = self.stream.clone_htod(args)?;
        let mut result_buffer = self.stream.alloc_zeros::<f32>(args.len())?;
        {
            let (args, _args_read) = arg_buffer.device_ptr(&self.stream);
            let (results, _results_written) = result_buffer.device_ptr_mut(&self.stream);
            let params = MathParams {
                args,
                results,
                count,
                function: u32::from(function),
            };
            self.launch(DeviceCopy(params), count, &self.math_binding_table())?;
        }
        Ok(self.stream.clone_dtoh(&result_buffer)?)
    }

    /// The shader binding table of a math launch: the math program's
    /// ray-generation record, with the triangle's miss and hit group
    /// records, which every table needs though the program traces no ray.
    fn math_binding_table(&self) -> ShaderBindingTable {
        self.binding_table(&self.math_raygen, Geometry::Triangle)
    }
}
